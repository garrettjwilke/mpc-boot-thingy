# mpc-boot-thingy. `make` cross-builds the device binaries into build/.
WARN = -Wall -Wextra
UI = mpc-ui-toolkit/src
INC = -Iinc -I$(UI)
DEV_SRC = src/main.c src/menu.c src/theme.c src/boot_ui.c \
	$(UI)/ui.c $(UI)/widgets.c $(UI)/input.c $(UI)/screen.c $(UI)/surface.c
HOST_SRC = src/test.c src/menu.c src/theme.c src/boot_ui.c $(UI)/ui.c $(UI)/widgets.c $(UI)/input.c

.PHONY: all test clean web-host

all: build/mpc-boot-thingy build/mpc-boot-web

build/mpc-boot-thingy: $(DEV_SRC) inc/menu.h inc/theme.h inc/boot_ui.h
	mkdir -p build
	zig cc -target aarch64-linux-musl -static -O2 -s -D_DEFAULT_SOURCE -std=c11 $(WARN) $(INC) \
	  -o $@ $(DEV_SRC)

build/mpc-boot-web: webserver/server.c src/menu.c inc/menu.h
	mkdir -p build
	zig cc -target aarch64-linux-musl -static -O2 -s $(WARN) -Iinc \
	  -o $@ webserver/server.c src/menu.c

build/mpc-boot-web-host: webserver/server.c src/menu.c inc/menu.h
	mkdir -p build
	cc -D_DARWIN_C_SOURCE -D_GNU_SOURCE -g $(WARN) -Iinc -o $@ webserver/server.c src/menu.c

web-host: build/mpc-boot-web-host

build/menu-test: $(HOST_SRC) inc/menu.h inc/theme.h inc/boot_ui.h
	mkdir -p build
	cc -std=c11 -D_DEFAULT_SOURCE -g $(WARN) $(INC) -o $@ $(HOST_SRC)

test: build/menu-test
	./build/menu-test
	sh test_setup.sh

clean:
	rm -rf build
