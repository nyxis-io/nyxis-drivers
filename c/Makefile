CC      = cc
CFLAGS  = -std=c99 -O2 -Wall -Wextra
LDFLAGS = -lm

.PHONY: all clean test bench

all: test bench

test: test.c nxs.c nxs.h nxs_writer.c nxs_writer.h
	$(CC) $(CFLAGS) -o test test.c nxs.c nxs_writer.c $(LDFLAGS)

bench: bench.c nxs.c nxs.h
	$(CC) $(CFLAGS) -o bench bench.c nxs.c $(LDFLAGS)

clean:
	rm -f test bench
