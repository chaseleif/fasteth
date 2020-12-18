CC=gcc
CFLAGS=-std=c99 -Wall -O3 -march=native -m64 -D_POSIX_C_SOURCE=200809L
BINS=fastserv fastcl
all: $(BINS)

.PHONY: fastserv fastcl

fastcl: fastcl.c common.c
	$(CC) $(CFLAGS) -o $@ $^

fastserv: fastserv.c common.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f ./fastcl ./fastserv

test: fastcl fastserv
	make -j runserver runclient

runserver: fastserv
	./fastserv -p 52528 #-out=./logs/server.log

runclient: fastcl
	./fastcl -n 36 127.0.0.1:52528 -in=./inputs/input #-out=./logs/client
