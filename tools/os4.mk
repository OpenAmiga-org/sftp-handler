# AmigaOS 4 build rules, run inside the cross-compiler image by
# tools/build_os4.sh. The handler is built with clib4 (needs clib4.library
# at run time), sftphprefs with newlib.

SDK      = /opt/ppc-amigaos/ppc-amigaos/SDK
CC       = ppc-amigaos-gcc
STRIP    = ppc-amigaos-strip
OBJCOPY  = ppc-amigaos-objcopy
# libssh2 with AmiSSL from tools/build_deps_os4.sh
SSH2DIR  = build/deps/libssh2-amissl
AMISSL   = build/deps/amissl-sdk/AmiSSL/Developer
WARN     = -Wall -Wextra -Werror -Wno-format-truncation
CFLAGS   = -O2 -g $(WARN) -std=gnu11 -mcrt=clib4 \
           -Isrc/core -Isrc/os4 -I$(SSH2DIR)/include -I$(AMISSL)/include \
           -I$(SDK)/local/common/include
LDFLAGS  = -mcrt=clib4 -athread=native -L$(SSH2DIR)/lib -L$(AMISSL)/lib/AmigaOS4 \
           -Lbuild/deps/amissl-clib4 -L$(SDK)/local/clib4/lib
LIBS     = -lssh2 -lamisslstubs -lamisslauto -lexpat
STRIPFLAGS = -R.comment --strip-unneeded-rel-relocs

CORE_SRC = src/core/util.c src/core/charset.c src/core/config.c src/core/net.c \
           src/core/ftplist.c src/core/ftp.c src/core/sftp.c src/core/sfs.c
OS4_SRC  = src/os4/main.c src/os4/platform_os4.c src/os4/requesters.c src/os4/amissl.c
# The filesysbox callbacks run in other tasks and must not call the C
# library, so keep the compiler from turning loops into library calls.
GLUE_SRC = src/os4/fbxglue.c
GLUE_FLAGS = -fno-builtin -fno-tree-loop-distribute-patterns

PREFS_SRC = src/prefs/sftphprefs.c src/core/config.c src/core/util.c src/core/charset.c
PREFS_CFLAGS = -O2 -g $(WARN) -std=gnu11 -mcrt=newlib -Isrc/core -Isrc/prefs \
           -I$(SDK)/local/common/include
PREFS_LDFLAGS = -mcrt=newlib -L$(SDK)/local/newlib/lib

.PHONY: handler prefs nettest

handler: build/os4/sftp-handler

build/os4/fbxglue.o: $(GLUE_SRC) $(wildcard src/core/*.h src/os4/*.h)
	$(CC) $(CFLAGS) $(GLUE_FLAGS) -c -o $@ $(GLUE_SRC)

build/os4/sftp-handler: $(CORE_SRC) $(OS4_SRC) build/os4/fbxglue.o $(wildcard src/core/*.h src/os4/*.h)
	$(CC) $(CFLAGS) -o $@.link $(CORE_SRC) $(OS4_SRC) build/os4/fbxglue.o $(LDFLAGS) $(LIBS)
	# AmigaOS starts a program at the symbol _start. Make that our sfh_entry,
	# which runs before clib4's startup code (see src/os4/main.c).
	$(OBJCOPY) --redefine-sym _start=clib4_crt_start --redefine-sym sfh_entry=_start $@.link $@.debug
	rm -f $@.link
	$(STRIP) $(STRIPFLAGS) -o $@ $@.debug

prefs: build/os4/sftphprefs

build/os4/sftphprefs: $(PREFS_SRC) $(wildcard src/core/*.h src/prefs/*.h)
	$(CC) $(PREFS_CFLAGS) -o $@.debug $(PREFS_SRC) $(PREFS_LDFLAGS) -lexpat -lauto
	$(STRIP) $(STRIPFLAGS) -o $@ $@.debug

nettest: build/os4/nettest

build/os4/nettest: tests/os4/nettest.c
	$(CC) $(CFLAGS) -Wno-error -o $@ $< $(LDFLAGS) -lssh2 -lamisslstubs -lamisslauto
