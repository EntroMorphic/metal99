# Host-side render harness. Strict ISO C99, same standard as the device code.
CFLAGS = -std=c99 -pedantic-errors -Wall -Wextra -O2 -I../../metal99/src
host_render: host_render.c vec_host.c
	$(CC) $(CFLAGS) $^ -o $@
clean:
	rm -f host_render *.ppm
.PHONY: clean
