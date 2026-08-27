# Host-side harness. Strict ISO C99, same standard AND warnings as the device
# code - -Wshadow included, because a shadowed variable in selftest.c is what
# made selftest_transport() incapable of reporting a failure.
CFLAGS = -std=c99 -pedantic-errors -Wall -Wextra -Wshadow -Werror -O2 \
         -I../../metal99/src
FOLD   = ../../metal99/src/fold.c
RGB    = ../../metal99/src/rgb565.c

# Every target depends on every firmware header. Coarse on purpose: without it,
# editing tile.h left a stale tile_test reporting the OLD tile geometry as if it
# had rebuilt - a test that does not rebuild is a test that lies, and it lied
# convincingly for one full measurement.
HDRS   = $(wildcard ../../metal99/src/*.h) $(wildcard *.h)

all: host_render digest_test gfx_test ui_test vg_test game_test tile_test gfx_png vg_png

host_render: host_render.c vec_host.c $(FOLD) $(RGB) $(HDRS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@

# Links metal99/src/fold.c: the assertions run against the firmware's own
# digest, not a host reimplementation of it.
digest_test: digest_test.c vec_host.c $(FOLD) $(HDRS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@

# gfx_test links the real metal99/src/gfx.c and stubs only the layers below it,
# so the run model under test is the firmware's own.
gfx_test: gfx_test.c gfx_stubs.c ../../metal99/src/gfx.c ../../metal99/src/elide.c \
          ../../metal99/src/font_share.c vec_host.c $(FOLD) $(RGB) $(RGB) $(HDRS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@

# Renders scenes to images so a change can be LOOKED at without a panel.
gfx_png: gfx_png.c gfx_stubs.c ../../metal99/src/gfx.c ../../metal99/src/elide.c \
         ../../metal99/src/ui.c ../../metal99/apps/demo.c \
         ../../metal99/src/font_share.c vec_host.c $(FOLD) $(RGB) $(HDRS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@

game_png: game_png.c sfx_stub.c ../../metal99/apps/gridvoid.c ../../metal99/src/vg.c \
          ../../metal99/src/trig.c ../../metal99/src/elide.c ../../metal99/src/tile.c \
          vec_host.c $(RGB) $(HDRS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@

gamepng: game_png $(HDRS)
	./game_png $(FRAMES)
	@python3 ../../tools/topng.py out_game.ppm

vg_png: vg_png.c ../../metal99/src/vg.c ../../metal99/src/trig.c vec_host.c $(HDRS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@

vgpng: vg_png $(HDRS)
	./vg_png
	@python3 ../../tools/topng.py out_vg.ppm

png: gfx_png $(HDRS)
	./gfx_png $(FRAMES)
	@python3 ../../tools/topng.py out_ideal.ppm out_panel.ppm out_diff.ppm

ui_test: ui_test.c gfx_stubs.c ../../metal99/src/ui.c ../../metal99/src/gfx.c \
         ../../metal99/src/elide.c ../../metal99/apps/demo.c \
         ../../metal99/src/font_share.c vec_host.c $(FOLD) $(RGB) $(HDRS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@

vg_test: vg_test.c ../../metal99/src/vg.c ../../metal99/src/trig.c vec_host.c $(HDRS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@

# ASAN where the toolchain has it: the rasteriser indexes arrays with computed
# row numbers, and that is where an off-by-one stops being cosmetic.
ASAN := $(shell echo 'int main(void){return 0;}' > /tmp/.asan$$$$.c && \
         $(CC) -fsanitize=address /tmp/.asan$$$$.c -o /dev/null 2>/dev/null && \
         echo -fsanitize=address; rm -f /tmp/.asan$$$$.c)

tile_test: tile_test.c sfx_stub.c ../../metal99/apps/gridvoid.c ../../metal99/src/vg.c \
           ../../metal99/src/trig.c ../../metal99/src/elide.c ../../metal99/src/tile.c \
           vec_host.c $(RGB) $(HDRS)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@

game_test: game_test.c sfx_stub.c ../../metal99/apps/gridvoid.c ../../metal99/src/vg.c \
           ../../metal99/src/trig.c ../../metal99/src/elide.c ../../metal99/src/tile.c \
           vec_host.c $(RGB) $(HDRS)
	$(CC) $(CFLAGS) $(ASAN) $(filter %.c,$^) -o $@

test: digest_test gfx_test ui_test vg_test game_test tile_test $(HDRS)
	./digest_test
	./gfx_test
	./ui_test
	./vg_test
	./game_test
	./tile_test
	@../../tools/c99check.sh

clean: $(HDRS)
	rm -f host_render digest_test gfx_test ui_test vg_test game_test tile_test gfx_png vg_png game_png *.ppm *.png

.PHONY: all clean test png vgpng gamepng
