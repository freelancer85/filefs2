#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include "fs.h"

unsigned char* fs;

#define TOTAL_BLOCKS (FSSIZE / BLKSIZE)
#define BLOCK_ENTRIES (BLKSIZE / sizeof(struct entry))

enum sector_types {SUPER, FREELIST, INODES, DATA, SECTOR_COUNT};
enum entry_types { E_FILE = 0, E_DIR};

struct sector { // sector describing area on the disk
  unsigned int sector_start;  //start sector
  unsigned int sector_size;   //number of blocks in sector
};

struct metadata {  //metadata found in super block
	unsigned int total_blocks;
  unsigned int total_inodes;
	unsigned int block_bytes;

  struct sector sectors[SECTOR_COUNT];  //sectors in filesystem
};

struct inode {  //inode in filesystem
	unsigned short dref[DREFSIZE]; //direct references
  unsigned short iref;           //indirect reference block
	unsigned char  total_ref;      //total references
};

struct entry {  // filesystem entry
  char             name[NAMESIZE];
	unsigned int     size;
  enum entry_types type;
  unsigned int     inode;
};

/* section pointers */
static struct metadata    * meta    = NULL;
static unsigned char      * bitlist = NULL;
static struct inode       * inodes  = NULL;

/* Helper functions */
static void * block_ref(unsigned int n) { return &fs[BLKSIZE * n]; }

/* Print n space on a line */
static void print_indent(int n){
  while(n-- > 0){
    printf(" ");
  }
}

/* Macro used to run through each block in an inode */
#define FOREACH_BLOCK(inode_ptr) \
  int i; \
  const unsigned short *indirect = (unsigned short *) block_ref(inode_ptr->iref); \
\
  for(i=0; i < inode_ptr->total_ref; i++){ \
    const unsigned int block = (i < DREFSIZE) ? inode_ptr->dref[i] : indirect[i - DREFSIZE];

/* Macro used to run through each entry in a directory */
#define FOREACH_ENTRY(inode_ptr) \
  int j; \
  FOREACH_BLOCK(inode_ptr) \
    struct entry * entry_ptr = (struct entry *) block_ref(block); \
    for(j=0; j < BLOCK_ENTRIES; j++, entry_ptr++)


/* Bit list manipulation */
static void bitlist_up(    unsigned int n){         bitlist[n / 8] |=  (1 << (n % 8)); }
static void bitlist_down(  unsigned int n){         bitlist[n / 8] &= ~(1 << (n % 8)); }
static int  bitlist_status(unsigned int n){ return (bitlist[n / 8] &   (1 << (n % 8)));}

/* Get a free data block */
static unsigned int get_data_block(){
  unsigned int i;
  for(i = meta->sectors[DATA].sector_start; i < meta->total_blocks; i++){
    if(bitlist_status(i) == 0){
      bitlist_up(i);
      break;
    }
  }
  return i;
}

/* Get a free inode */
static unsigned int get_inode(){
  unsigned int i;
  for(i=0; i < TOTAL_INODES; i++){
    if(inodes[i].total_ref == 0){
      break;
    }
  }
  return i;
}

/* Search for an entry by name */
static struct entry* search_entry(struct inode * inode_ptr, const char * name){

  FOREACH_ENTRY(inode_ptr){
      if(strcmp(name, entry_ptr->name) == 0){
        return entry_ptr;
      }
    }
  }
  return NULL;
}

/* Expand inode, using the indirect references */
static int expand_indirect(struct inode * inode_ptr, const unsigned int block){
  if(inode_ptr->iref == 0){
    const unsigned int iref = get_data_block();
    if(iref == meta->total_blocks){
      fprintf(stderr, "Error: Enlarge failed, no blocks\n");
      return -1;
    }
    inode_ptr->iref = iref;
  }

  unsigned short *indirect = (unsigned short *) block_ref(inode_ptr->iref);
  indirect[inode_ptr->total_ref - DREFSIZE] = block;

  return 0;
}

/* Expand inode, using the direct references */
static int expand(struct inode * inode_ptr){

  int block = get_data_block();
  if(block == meta->total_blocks){ //if a free block wasn't found
    fprintf(stderr, "Error: Enlarge failed, no free blocks\n");
    return -1;
  }

  if(inode_ptr->total_ref < DREFSIZE){
    inode_ptr->dref[inode_ptr->total_ref] = block;
  }else{
    expand_indirect(inode_ptr, block);
  }
  inode_ptr->total_ref++;
  return block;
}

/* Add entry by name, or return existing entry */
static struct entry* get_entry(struct entry * entry_ptr, const char * name){
  struct inode * inode_ptr = &inodes[entry_ptr->inode];
  struct inode * einode_ptr = NULL;

  /* if entry exist */
  entry_ptr = search_entry(inode_ptr, name);
  if(entry_ptr != NULL){
    return entry_ptr;
  }

  /* find free entry in dir to store */
  entry_ptr = search_entry(inode_ptr, "");
  if(entry_ptr == NULL){
    if(expand(inode_ptr) == -1){
      return NULL;
    }
    entry_ptr = search_entry(inode_ptr, "");
  }

  /* find inode */
  const int inode = get_inode();
  if(inode == TOTAL_INODES){
    return NULL;
  }
  einode_ptr = &inodes[inode];

  bzero(einode_ptr, sizeof(struct inode));

  /* assign data block to inode */
  if(expand(einode_ptr) == -1){
    return NULL;
  }

  /* store entry data */
  strncpy(entry_ptr->name, name, NAMESIZE);
  entry_ptr->inode = inode;
  entry_ptr->type = E_DIR;
  entry_ptr->size = 0;

  return entry_ptr;
}

/* Write data to entry */
static int write_entry(struct entry * entry_ptr, const int fd){
  int i=0;
  struct inode * inode_ptr = &inodes[entry_ptr->inode];
  unsigned char * block_ptr = block_ref(inode_ptr->dref[0]);

  entry_ptr->size = 0;

  while(1){

    /* read from file to block buffer */
    const int n = read(fd, &block_ptr[i], BLKSIZE - i);
    if(n < 0){
      perror("read");
      break;
    }else if(n == 0){
      break;
    }
    /* increase entry size */
    entry_ptr->size += n;

    /* if we have read a whole block */
    if(i == BLKSIZE){
      i = 0;
      /* get another one */
      const unsigned int block = expand(inode_ptr);
      if(block == -1){  //if not free block
        break;
      }
      block_ptr = block_ref(block);
    }else{
      /* advance block pointer index with bytes read */
      i += n;
    }
  }
  return entry_ptr->size;
}

/* Read data from entry to file */
static int entry_read(struct entry * entry_ptr, FILE * out){
  struct inode * inode_ptr = &inodes[entry_ptr->inode];
  int size = entry_ptr->size;

  FOREACH_BLOCK(inode_ptr)
    const int n = (size > BLKSIZE) ? BLKSIZE : size;
    size -= n;
    fwrite(block_ref(block), 1, n, out);
  }
  return 0;
}


/* Remove entry from a directory */
static void entry_remove(struct entry * entry_ptr){

  struct inode * inode_ptr = &inodes[entry_ptr->inode];

  /* release each data block hold by inode */
  FOREACH_BLOCK(inode_ptr)
    bitlist_down(block);
  }

  if(inode_ptr->iref > 0){
    /* release indirect data block hold by inode */
    bitlist_down(inode_ptr->iref);
  }

  /* zero out entry and inode memory */
  bzero(inode_ptr, sizeof(struct inode));
  bzero(entry_ptr, sizeof(struct entry));
}

/* Returns number of entries in a directory */
static int entry_count(struct inode *inode_ptr){
  int n = 0;
  FOREACH_ENTRY(inode_ptr){
      if(entry_ptr->inode != 0){
        ++n;
      }
    }
  }
  return n;
}

/* Remove entry by following a path */
static void entry_remove_path(struct inode * inode_ptr, char * path){

  /* On first call, it will load path to strtok, on next it will give subdirs */
  const char * name = strtok(path, "/");
  if(name == NULL){
    return;
  }

  struct entry * entry_ptr = search_entry(inode_ptr, name);
  if(entry_ptr == NULL){
    fprintf(stderr, "Error: Entry '%s' not found\n", name);
    return;
  }

  if(entry_ptr->type == E_DIR){ /* if its a subdir */
    struct inode * einode_ptr = &inodes[entry_ptr->inode];
    /* recurse into the directory */
    entry_remove_path(einode_ptr, NULL);

    if(entry_count(einode_ptr) == 0){ /* if dir is empry after file deleted */
      entry_remove(entry_ptr);    /* remove it */
    }

  }else if(entry_ptr->type == E_FILE){
    entry_remove(entry_ptr);
  }
}

/* List entries in a directory */
static void entry_list(struct inode * inode_ptr, const int level){

  FOREACH_ENTRY(inode_ptr){
      if(entry_ptr->inode == 0){
        continue;
      }

      print_indent(level);

      switch(entry_ptr->type){
        case E_FILE:
          printf("'%s' %d\n", entry_ptr->name, entry_ptr->size);
          break;
        case E_DIR:
          printf("directory '%s':\n", entry_ptr->name);
          entry_list(&inodes[entry_ptr->inode], level+1);
          break;
        default:
          break;
      }
    }
  }
}

void mapfs(int fd){
  if ((fs = mmap(NULL, FSSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == NULL){
      perror("mmap failed");
      exit(EXIT_FAILURE);
  }
}


void unmapfs(){
  munmap(fs, FSSIZE);
}

void setup_sectors(){
  meta->total_blocks = TOTAL_BLOCKS;
  meta->total_blocks = TOTAL_INODES;
  meta->block_bytes  = BLKSIZE;

  // superblock takes 1 block
  meta->sectors[SUPER].sector_start = 0;
  meta->sectors[SUPER].sector_size = 1;

  // bitmap takes 1 bit for each block
  meta->sectors[FREELIST].sector_start = meta->sectors[SUPER].sector_size;
  meta->sectors[FREELIST].sector_size = (TOTAL_BLOCKS / 8) / BLKSIZE;

  //inodes are 100
  meta->sectors[INODES].sector_start = meta->sectors[FREELIST].sector_start + meta->sectors[FREELIST].sector_size;
  meta->sectors[INODES].sector_size  = TOTAL_INODES / (BLKSIZE / sizeof(struct inode));

  //data is at end
  meta->sectors[DATA].sector_start = meta->sectors[INODES].sector_start + meta->sectors[INODES].sector_size;
  meta->sectors[DATA].sector_size  = meta->total_blocks - (meta->sectors[FREELIST].sector_size + meta->sectors[INODES].sector_size);
}

static void create_root(){
  struct inode * inode_ptr = &inodes[0];
  const int block = expand(inode_ptr);
  struct entry * entry_ptr = (struct entry *)block_ref(block);

  entry_ptr->name[0] = '/';
  entry_ptr->type   = E_DIR;
  entry_ptr->inode  = 0;
  entry_ptr->size   = 0;
}

void formatfs(){
  int i;

  bzero(fs, FSSIZE);

  /* save metadata info*/
  meta = (struct metadata*) fs;

  setup_sectors();
  loadfs();

  /* setup system blocks as used in bit list*/
  bitlist_up(0);
  for(i=0; i < meta->sectors[FREELIST].sector_size; i++){
    bitlist_up(meta->sectors[FREELIST].sector_start + i);
  }

  for(i=0; i < meta->sectors[INODES].sector_size; i++){
    bitlist_up(meta->sectors[INODES].sector_start + i);
  }

  /* create the / directory */
  create_root();
}


void loadfs(){
  meta    = (struct metadata*) fs;
  bitlist = (unsigned char*) block_ref(meta->sectors[FREELIST].sector_start);
  inodes  = (struct inode*)  block_ref(meta->sectors[INODES].sector_start);
}

void lsfs(){
  struct inode * inode_ptr = &inodes[0];
  entry_list(inode_ptr, 0);
}


void addfilefs(char* fname){

  struct inode * inode_ptr = &inodes[0];
  struct entry * entry_ptr = (struct entry *) block_ref(inode_ptr->dref[0]);

  /* open input file */
  const int fd = open(fname, O_RDONLY);
  if(fd == -1){
    perror("open");
    return;
  }

  /* go down the path to file */
  char * name = strtok(fname, "/");
  while(name){
    /* get/create entry for this subdir */
    entry_ptr = get_entry(entry_ptr, name);
    if( (entry_ptr == NULL) ||
        (entry_ptr->type == E_FILE)){
      fprintf(stderr, "Error: Invalid subdir %s\n", name);
      return;
    }
    name = strtok(NULL, "/");
  }

  /* write file data to entry */
  write_entry(entry_ptr, fd);
  /* set entry to be a file */
  entry_ptr->type = E_FILE;

  close(fd);
}

void removefilefs(char* fname){
  entry_remove_path(&inodes[0], fname);
}

void extractfilefs(char* fname){
  //start from root directory
  struct inode * inode_ptr = &inodes[0];
  struct entry * entry_ptr = NULL;

  /* go down the path to file */
  char * name = strtok(fname, "/");
  while(name){

    /* search for the name in this directory */
    entry_ptr = search_entry(inode_ptr, name);
    if(entry_ptr == NULL){
      fprintf(stderr, "Error: Not found\n");
      return;
    }

    if(entry_ptr->type == E_DIR){
      /* go to next directory */
      inode_ptr = &inodes[entry_ptr->inode];
    }else{
      break;
    }
    name = strtok(NULL, "/");
  }

  /* output to stdout */
  entry_read(entry_ptr, stdout);
}

static void entry_debug(struct inode * inode_ptr, int indent, char * name){

  if(name == NULL){
    return;
  }

  //First list all of the entries
  FOREACH_ENTRY(inode_ptr){
      if(entry_ptr->inode == 0){
        continue;
      }

      struct inode * einode_ptr = &inodes[entry_ptr->inode];

      print_indent(indent + 1);
      switch(entry_ptr->type){
        case E_FILE:
          if(strcmp(entry_ptr->name, name) == 0){
            printf("'%s' %d inode=%d\n", entry_ptr->name, entry_ptr->size, entry_ptr->inode);
            return;
          }
          break;
        case E_DIR:
          printf("directory '%s' inode=%d:\n", entry_ptr->name, entry_ptr->inode);
          if(strcmp(entry_ptr->name, name) == 0){
            name = strtok(NULL, "/");
            entry_debug(einode_ptr, indent + 1, name);
            return;
          }
          break;
        default:
          break;
      }
    }
  }

}

void debugfs(char * fname){
  struct inode * inode_ptr = &inodes[0];
  struct entry * entry_ptr = (struct entry *) block_ref(inode_ptr->dref[0]);

  char * name = strtok(fname, "/");

  entry_debug(&inodes[entry_ptr->inode], 0, name);
}
