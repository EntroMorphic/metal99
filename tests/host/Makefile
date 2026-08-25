# Host-side harness. Strict ISO C99, same standard AND warnings as the device
# code - -Wshadow included, because a shadowed variable in selftest.c is what
# made selftest_transport() incapable of reporting a failure.
CFLAGS = -std=c99 -pedantic-errors -Wall -Wextra -Wshadow -Werror -O2 \
         -I../../metal99/src
FOLD   = ../../metal99/src/fold.c
RGB    = ../../metal99/src/rgb565.c

all: host_render digest_test gfx_test ui_test vg_test game_test gfx_png vg_png

host_render: host_render.c vec_host.c $(FOLD) $(RGB)
	$(CC) $(CFLAGS) $^ -o $@

# Links metal99/src/fold.c: the assertions run against the firmware's own
# digest, not a host reimplementation of it.
digest_test: digest_test.c vec_host.c $(FOLD)
	$(CC) $(CFLAGS) $^ -o $@

# gfx_test links the real metal99/src/gfx.c and stubs only the layers below it,
# so the run model under test is the firmware's own.
gfx_test: gfx_test.c gfx_stubs.c ../../metal99/src/gfx.c ../../metal99/src/elide.c \
          ../../metal99/src/font_share.c vec_host.c $(FOLD) $(RGB) $(RGB)
	$(CC) $(CFLAGS) $^ -o $@

# Renders scenes to images so a change can be LOOKED at without a panel.
gfx_png: gfx_png.c gfx_stubs.c ../../metal99/src/gfx.c ../../metal99/src/elide.c \
         ../../metal99/src/ui.c ../../metal99/apps/demo.c \
         ../../metal99/src/font_share.c vec_host.c $(FOLD) $(RGB)
	$(CC) $(CFLAGS) $^ -o $@

game_png: game_png.c ../../metal99/apps/gridvoid.c ../../metal99/src/vg.c \
          ../../metal99/src/trig.c ../../metal99/src/elide.c vec_host.c $(RGB)
	$(CC) $(CFLAGS) $^ -o $@

gamepng: game_png
	./game_png $(FRAMES)
	@python3 ../../tools/topng.py out_game.ppm

vg_png: vg_png.c ../../metal99/src/vg.c ../../metal99/src/trig.c vec_host.c
	$(CC) $(CFLAGS) $^ -o $@

vgpng: vg_png
	./vg_png
	@python3 ../../tools/topng.py out_vg.ppm

png: gfx_png
	./gfx_png $(FRAMES)
	@python3 ../../tools/topng.py out_ideal.ppm out_panel.ppm out_diff.ppm

ui_test: ui_test.c gfx_stubs.c ../../metal99/src/ui.c ../../metal99/src/gfx.c \
         ../../metal99/src/elide.c ../../metal99/apps/demo.c \
         ../../metal99/src/font_share.c vec_host.c $(FOLD) $(RGB)
	$(CC) $(CFLAGS) $^ -o $@

vg_test: vg_test.c ../../metal99/src/vg.c ../../metal99/src/trig.c vec_host.c
	$(CC) $(CFLAGS) $^ -o $@

# ASAN where the toolchain has it: the rasteriser indexes arrays with computed
# row numbers, and that is where an off-by-one stops being cosmetic.
ASAN := $(shell echo 'int main(void){return 0;}' > /tmp/.asan$$$$.c && \
         $(CC) -fsanitize=address /tmp/.asan$$$$.c -o /dev/null 2>/dev/null && \
         echo -fsanitize=address; rm -f /tmp/.asan$$$$.c)

game_test: game_test.c ../../metal99/apps/gridvoid.c ../../metal99/src/vg.c \
           ../../metal99/src/trig.c ../../metal99/src/elide.c vec_host.c $(RGB)
	$(CC) $(CFLAGS) $(ASAN) $^ -o $@

test: digest_test gfx_test ui_test vg_test game_test
	./digest_test
	./gfx_test
	./ui_test
	./vg_test
	./game_test
	@../../tools/c99check.sh

clean:
	rm -f host_render digest_test gfx_test ui_test vg_test game_test gfx_png vg_png game_png *.ppm *.png

.PHONY: all clean test png vgpng gamepng
