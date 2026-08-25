# Host-side harness. Strict ISO C99, same standard AND warnings as the device
# code - -Wshadow included, because a shadowed variable in selftest.c is what
# made selftest_transport() incapable of reporting a failure.
CFLAGS = -std=c99 -pedantic-errors -Wall -Wextra -Wshadow -Werror -O2 \
         -I../../metal99/src
FOLD   = ../../metal99/src/fold.c

all: host_render digest_test

host_render: host_render.c vec_host.c $(FOLD)
	$(CC) $(CFLAGS) $^ -o $@

# Links metal99/src/fold.c: the assertions run against the firmware's own
# digest, not a host reimplementation of it.
digest_test: digest_test.c vec_host.c $(FOLD)
	$(CC) $(CFLAGS) $^ -o $@

test: digest_test
	./digest_test

clean:
	rm -f host_render digest_test *.ppm

.PHONY: all clean test
