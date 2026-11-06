CC ?= gcc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation
SRC = src/ui_main.c src/tools.c src/canvas.c src/ui.c src/gfx.c src/imgio.c src/jpeg.c src/resdata.c src/font_data.c
PLAT_X11 = src/plat_x11.c
LIBS = -lX11 -l:libXi.so.6 -lcairo -lm

mspaint: $(SRC) $(PLAT_X11) $(wildcard src/*.h)
	$(CC) $(CFLAGS)  -o $@ $(SRC) $(PLAT_X11) $(LIBS)

mspaint-test: $(SRC) $(PLAT_X11) $(wildcard src/*.h)
	$(CC) $(CFLAGS) -DPAINT_TEST  -g -o $@ $(SRC) $(PLAT_X11) $(LIBS)

clean:
	rm -f mspaint mspaint-test
