CC = gcc
CFLAGS = -Wall -g

build: application

application: server.o sock_util.o
	$(CC) $^ -laio -lsqlite3 -o $@

server.o: server.c server.h
	$(CC) $(CFLAGS) -c $< -o $@

sock_util.o: sock_util.c sock_util.h
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean

clean:
	rm -rf *.o application
