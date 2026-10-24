# mpc-boot-thingy. `make` cross-builds the device binary into build/.
WARN = -Wall -Wextra
INC = -Iinc -I../mpc-sample-lab/panel-demo -I../mpc-sample-lab/firmware

.PHONY: test clean

build/mpc-boot-thingy: src/main.c src/menu.c src/draw.c src/screen.c inc/menu.h inc/draw.h inc/screen.h
	mkdir -p build
	zig cc -target aarch64-linux-musl -static -O2 -s $(WARN) $(INC) \
	  -o $@ src/main.c src/menu.c src/draw.c src/screen.c

build/menu-test: src/test.c src/menu.c inc/menu.h
	mkdir -p build
	cc -std=c11 -D_DEFAULT_SOURCE -g $(WARN) -Iinc -o $@ src/test.c src/menu.c

test: build/menu-test
	./build/menu-test
	sh test_setup.sh

clean:
	rm -rf build
