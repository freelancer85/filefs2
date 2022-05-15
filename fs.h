#ifndef __FS_H__
#define __FS_H__
#include <sys/mman.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#define FSSIZE 10000000
#define BLKSIZE 512
#define NAMESIZE 255
#define TOTAL_INODES 100
#define DREFSIZE 100

extern unsigned char* fs;

void mapfs(int fd);
void unmapfs();
void formatfs();
void loadfs();
void lsfs();
void addfilefs(char* fname);
void removefilefs(char* fname);
void extractfilefs(char* fname);
void debugfs(char * fname);

#endif
