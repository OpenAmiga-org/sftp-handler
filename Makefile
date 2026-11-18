# sftp-handler
#
#   make linux      Linux FUSE test build  -> build/linux/sftph-fuse
#   make os4        AmigaOS 4 handler and prefs (cross-compiled in the
#                   walkero/amigagccondocker image)
#   make test       Linux build + unit tests + tests against testserver/
#   make lha        release archive         -> dist/SFTPHandler.lha

CORE_SRC  = src/core/util.c src/core/charset.c src/core/config.c src/core/net.c \
            src/core/ftplist.c src/core/ftp.c src/core/sftp.c src/core/sfs.c
CORE_HDR  = $(wildcard src/core/*.h)

CC        ?= gcc
CFLAGS    ?= -O2 -g
WARN       = -Wall -Wextra -Werror -Wno-format-truncation -std=gnu11
PKGS       = fuse3 expat
# the same patched libssh2 as the AmigaOS build (tools/libssh2-patches)
SSH2       = build/deps/libssh2-linux
SSH2_LIB   = $(SSH2)/lib/libssh2.a

.PHONY: all linux os4 test unit lha clean

all: linux

linux: build/linux/sftph-fuse

$(SSH2_LIB): $(wildcard tools/libssh2-patches/*.patch) tools/build_deps_linux.sh
	./tools/build_deps_linux.sh

build/linux/sftph-fuse: $(CORE_SRC) $(CORE_HDR) src/linux/main.c src/linux/platform_linux.c $(SSH2_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(WARN) -Isrc/core -I$(SSH2)/include $(shell pkg-config --cflags $(PKGS)) \
		-o $@ $(CORE_SRC) src/linux/main.c src/linux/platform_linux.c \
		$(SSH2_LIB) -lssl -lcrypto $(shell pkg-config --libs $(PKGS))

build/unit/unit_tests: tests/unit_tests.c src/core/ftplist.c src/core/config.c src/core/util.c src/core/charset.c $(CORE_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(WARN) -fsanitize=address,undefined -Isrc/core -o $@ \
		tests/unit_tests.c src/core/ftplist.c src/core/config.c src/core/util.c src/core/charset.c -lexpat

unit: build/unit/unit_tests
	build/unit/unit_tests

os4:
	./tools/build_os4.sh

test: linux unit
	python3 -I tests/run_tests.py

lha: os4
	./tools/make_lha.sh

clean:
	rm -rf build
