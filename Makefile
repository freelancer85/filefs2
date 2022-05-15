all:
	gcc -Wall -g fs.c filefs.c -o filefs

clean:
	rm -f filefs
