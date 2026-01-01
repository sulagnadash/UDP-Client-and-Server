CC=gcc
CFLAGS=-Wall -Iincludes -Wextra -ggdb
VPATH=src

all: client server

client: client.c

server: server.c

clean:
	rm -rf client server *.o


.PHONY : clean all
