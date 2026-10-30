# mpc-boot-thingy. `make` cross-builds the device binaries into build/.
WARN = -Wall -Wextra
UI = mpc-ui-toolkit/src
INC = -Iinc -I$(UI)
DEV_SRC = src/main.c src/menu.c src/boot_edit.c src/theme.c src/boot_ui.c src/png.c src/sd_check.c \
	$(UI)/ui.c $(UI)/widgets.c $(UI)/usb_sd.c $(UI)/input.c $(UI)/screen.c $(UI)/surface.c
HOST_SRC = src/test.c src/menu.c src/boot_edit.c src/theme.c src/boot_ui.c src/png.c src/sd_check.c \
	$(UI)/ui.c $(UI)/widgets.c $(UI)/usb_sd.c $(UI)/input.c

.PHONY: all test clean web-host

all: build/mpc-boot-thingy build/mpc-boot-web build/loader.so

build/mpc-boot-thingy: $(DEV_SRC) inc/menu.h inc/theme.h inc/boot_ui.h inc/boot_edit.h inc/sd_check.h $(UI)/usb_sd.h
	mkdir -p build
	zig cc -target aarch64-linux-musl -static -O2 -s -D_DEFAULT_SOURCE -std=c11 $(WARN) $(INC) \
	  -o $@ $(DEV_SRC)

build/poke: menu-patch/poke.c
	mkdir -p build
	zig cc -target aarch64-linux-musl -static -O2 -s -o $@ menu-patch/poke.c

build/loader.so: menu-patch/loader.c
	mkdir -p build
	zig cc -target aarch64-linux-gnu.2.34 -shared -fPIC -O2 -s -std=c11 $(WARN) \
	  -o $@ menu-patch/loader.c -ldl

build/loader-test: menu-patch/loader.c
	mkdir -p build
	cc -std=c11 -D_DEFAULT_SOURCE -DMPC_LOADER_TEST -g $(WARN) -o $@ menu-patch/loader.c

build/mpc-boot-web: webserver/server.c src/menu.c src/boot_edit.c inc/menu.h inc/boot_edit.h
	mkdir -p build
	zig cc -target aarch64-linux-musl -static -O2 -s $(WARN) -Iinc \
	  -o $@ webserver/server.c src/menu.c src/boot_edit.c

build/mpc-boot-web-host: webserver/server.c src/menu.c src/boot_edit.c inc/menu.h inc/boot_edit.h
	mkdir -p build
	cc -D_DARWIN_C_SOURCE -D_GNU_SOURCE -g $(WARN) -Iinc -o $@ webserver/server.c src/menu.c src/boot_edit.c

web-host: build/mpc-boot-web-host

build/menu-test: $(HOST_SRC) inc/menu.h inc/theme.h inc/boot_ui.h inc/boot_edit.h $(UI)/usb_sd.h
	mkdir -p build
	cc -std=c11 -D_DEFAULT_SOURCE -g $(WARN) $(INC) -o $@ $(HOST_SRC)

test: build/menu-test build/loader-test
	./build/menu-test
	./build/loader-test
	sh test_setup.sh

clean:
	rm -rf build
