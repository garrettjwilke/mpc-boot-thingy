# mpc-boot-thingy. `make` cross-builds the device binaries into build/
# and writes the testapp zip to dist/mpc-boot-thingy.zip.
WARN = -Wall -Wextra
APP_VERSION := $(shell head -n 1 VERSION | sed -e 's/[[:space:]]//g' -e 's/^v//')
UI = mpc-ui-toolkit/src
INC = -Iinc -Iwebserver -I$(UI)
DEV_SRC = src/main.c src/menu.c src/boot_edit.c src/theme.c src/boot_ui.c src/png.c src/sd_check.c src/format_sd.c src/testapp.c src/app_install.c src/usb_apps.c webserver/unzip.c \
	$(UI)/ui.c $(UI)/widgets.c $(UI)/usb_sd.c $(UI)/input.c $(UI)/screen.c $(UI)/surface.c
INSTALLER_SRC = src/usb_installer.c src/theme.c src/sd_check.c src/format_sd.c \
	$(UI)/ui.c $(UI)/widgets.c $(UI)/input.c $(UI)/screen.c $(UI)/surface.c
HOST_SRC = src/test.c src/menu.c src/boot_edit.c src/theme.c src/boot_ui.c src/png.c src/sd_check.c src/format_sd.c src/testapp.c src/app_install.c src/usb_apps.c \
	webserver/unzip.c webserver/file_ops.c webserver/zip_writer.c webserver/screen_capture.c \
	$(UI)/ui.c $(UI)/widgets.c $(UI)/usb_sd.c $(UI)/input.c

.PHONY: all test clean web-host package

all: build/mpc-boot-thingy build/mpc-boot-web build/loader.so build/usb_installer package

package: build/mpc-boot-thingy build/mpc-boot-web build/loader.so build/usb_installer
	mkdir -p dist
	./setup.sh --pack dist/mpc-boot-thingy.zip

build/mpc-boot-thingy: $(DEV_SRC) inc/menu.h inc/theme.h inc/boot_ui.h inc/boot_edit.h inc/sd_check.h inc/format_sd.h inc/testapp.h inc/app_install.h inc/usb_apps.h inc/unzip.h $(UI)/usb_sd.h VERSION
	mkdir -p build
	zig cc -target aarch64-linux-musl -static -O2 -s -D_DEFAULT_SOURCE -std=c11 $(WARN) $(INC) \
	  -DAPP_VERSION=\"$(APP_VERSION)\" -o $@ $(DEV_SRC)

build/usb_installer: $(INSTALLER_SRC) inc/theme.h inc/sd_check.h inc/format_sd.h VERSION
	mkdir -p build
	zig cc -target aarch64-linux-musl -static -O2 -s -D_DEFAULT_SOURCE -std=c11 $(WARN) $(INC) \
	  -DAPP_VERSION=\"$(APP_VERSION)\" -o $@ $(INSTALLER_SRC)

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

build/mpc-boot-web: webserver/server.c webserver/unzip.c webserver/file_ops.c webserver/zip_writer.c webserver/screen_capture.c src/menu.c src/boot_edit.c src/app_install.c src/png.c inc/menu.h inc/boot_edit.h inc/unzip.h inc/app_install.h inc/png.h webserver/file_ops.h webserver/zip_writer.h webserver/screen_capture.h
	mkdir -p build
	zig cc -target aarch64-linux-musl -static -O2 -s $(WARN) $(INC) \
	  -o $@ webserver/server.c webserver/unzip.c webserver/file_ops.c webserver/zip_writer.c webserver/screen_capture.c src/menu.c src/boot_edit.c src/app_install.c src/png.c

build/mpc-boot-web-host: webserver/server.c webserver/unzip.c webserver/file_ops.c webserver/zip_writer.c webserver/screen_capture.c src/menu.c src/boot_edit.c src/app_install.c src/png.c inc/menu.h inc/boot_edit.h inc/unzip.h inc/app_install.h inc/png.h webserver/file_ops.h webserver/zip_writer.h webserver/screen_capture.h
	mkdir -p build
	cc -D_DARWIN_C_SOURCE -D_GNU_SOURCE -g $(WARN) $(INC) -o $@ webserver/server.c webserver/unzip.c webserver/file_ops.c webserver/zip_writer.c webserver/screen_capture.c src/menu.c src/boot_edit.c src/app_install.c src/png.c

web-host: build/mpc-boot-web-host

build/menu-test: $(HOST_SRC) inc/menu.h inc/theme.h inc/boot_ui.h inc/boot_edit.h inc/sd_check.h inc/format_sd.h inc/testapp.h inc/app_install.h inc/usb_apps.h inc/unzip.h $(UI)/usb_sd.h VERSION
	mkdir -p build
	cc -std=c11 -D_DEFAULT_SOURCE -g $(WARN) $(INC) -DAPP_VERSION=\"$(APP_VERSION)\" -o $@ $(HOST_SRC)

test: build/menu-test build/loader-test
	./build/menu-test
	./build/loader-test
	sh test_setup.sh

clean:
	rm -rf build
