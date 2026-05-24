CC      = cc
CFLAGS  = -std=c99 -O2 -Wall -Wextra
LDFLAGS = -lm -pthread

.PHONY: all clean test test-prefetch bench profile-selective

all: test bench

test: test.c nxs.c nxs.h nxs_prefetch.c nxs_prefetch.h nxs_writer.c nxs_writer.h
	$(CC) $(CFLAGS) -o test test.c nxs.c nxs_prefetch.c nxs_writer.c $(LDFLAGS)
	@# Run via top-level `make test-c` (passes FIXTURE_OUT) or: ./test ../js/fixtures

test-prefetch: test_prefetch.c nxs.c nxs.h nxs_prefetch.c nxs_prefetch.h nxs_writer.c nxs_writer.h
	$(CC) $(CFLAGS) -o test_prefetch test_prefetch.c nxs.c nxs_prefetch.c nxs_writer.c $(LDFLAGS)
	./test_prefetch

bench: bench.c nxs.c nxs.h nxs_prefetch.c nxs_prefetch.h
	$(CC) $(CFLAGS) -o bench bench.c nxs.c nxs_prefetch.c $(LDFLAGS)

profile-selective: profile_selective.c nxs.c nxs.h nxs_prefetch.c nxs_prefetch.h
	$(CC) $(CFLAGS) -O3 -o profile-selective profile_selective.c nxs.c nxs_prefetch.c $(LDFLAGS)

clean:
	rm -f test test_prefetch bench profile-selective
