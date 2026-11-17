# AmigaOS 4 build rules, run inside the cross-compiler image by
# tools/build_os4.sh. Built with clib4 (needs clib4.library at run time).

SDK      = /opt/ppc-amigaos/ppc-amigaos/SDK
CC       = ppc-amigaos-gcc
STRIP    = ppc-amigaos-strip
OBJCOPY  = ppc-amigaos-objcopy
# libssh2 with AmiSSL from tools/build_deps_os4.sh
SSH2DIR  = build/deps/libssh2-amissl
AMISSL   = build/deps/amissl-sdk/AmiSSL/Developer
# AmiSSL 6 SDK: only for the offsets of its interface (amissl_off_v4.c)
AMISSL6  = build/deps/amissl6-sdk/AmiSSL/Developer
WARN     = -Wall -Wextra -Werror -Wno-format-truncation
CFLAGS   = -O2 -g $(WARN) -std=gnu11 -mcrt=clib4 \
           -Isrc/core -Isrc/os4 -I$(SSH2DIR)/include -I$(AMISSL)/include \
           -I$(SDK)/local/common/include
LDFLAGS  = -mcrt=clib4 -athread=native -L$(SSH2DIR)/lib -L$(AMISSL)/lib/AmigaOS4 \
           -Lbuild/deps/amissl-clib4 -L$(SDK)/local/clib4/lib
# no -lamisslstubs: its stubs only fit AmiSSL 5; build/os4/amissl_*.c (made
# by tools/gen_amissl_glue.py in tools/build_os4.sh) fit AmiSSL 5 and 6
LIBS     = -lssh2 -lamisslauto -lexpat
# BETA=1 (tools/build_os4.sh, off the main branch): marked as a beta
ifdef BETA
CFLAGS  += -DSFH_BETA
endif
STRIPFLAGS = -R.comment --strip-unneeded-rel-relocs

CORE_SRC = src/core/util.c src/core/charset.c src/core/config.c src/core/net.c \
           src/core/ftplist.c src/core/ftp.c src/core/sftp.c src/core/sfs.c
OS4_SRC  = src/os4/main.c src/os4/platform_os4.c src/os4/requesters.c src/os4/amissl.c
# The filesysbox callbacks run in other tasks and must not call the C
# library, so keep the compiler from turning loops into library calls.
GLUE_SRC = src/os4/fbxglue.c
GLUE_FLAGS = -fno-builtin -fno-tree-loop-distribute-patterns
AMISSL_OBJ = build/os4/amissl_glue.o build/os4/amissl_off_v3.o build/os4/amissl_off_v4.o

# sftphprefs tests connections with the handler's own code
PREFS_SRC = src/prefs/sftphprefs.c src/core/util.c src/core/charset.c src/core/config.c \
            src/core/net.c src/core/ftplist.c src/core/ftp.c src/core/sftp.c \
            src/os4/platform_os4.c src/os4/requesters.c src/os4/amissl.c

.PHONY: handler prefs nettest

handler: build/os4/sftp-handler

build/os4/fbxglue.o: $(GLUE_SRC) $(wildcard src/core/*.h src/os4/*.h)
	$(CC) $(CFLAGS) $(GLUE_FLAGS) -c -o $@ $(GLUE_SRC)

build/os4/amissl_glue.o: build/os4/amissl_glue.c
	$(CC) $(CFLAGS) -c -o $@ $<

build/os4/amissl_off_v3.o: build/os4/amissl_off_v3.c
	$(CC) $(CFLAGS) -c -o $@ $<

# the AmiSSL 6 headers first
build/os4/amissl_off_v4.o: build/os4/amissl_off_v4.c
	$(CC) -I$(AMISSL6)/include $(CFLAGS) -c -o $@ $<

build/os4/sftp-handler: $(CORE_SRC) $(OS4_SRC) build/os4/fbxglue.o $(AMISSL_OBJ) $(wildcard src/core/*.h src/os4/*.h)
	$(CC) $(CFLAGS) -o $@.link $(CORE_SRC) $(OS4_SRC) build/os4/fbxglue.o $(AMISSL_OBJ) $(LDFLAGS) $(LIBS)
	# AmigaOS starts a program at the symbol _start. Make that our sfh_entry,
	# which runs before clib4's startup code (see src/os4/main.c).
	$(OBJCOPY) --redefine-sym _start=clib4_crt_start --redefine-sym sfh_entry=_start $@.link $@.debug
	rm -f $@.link
	$(STRIP) $(STRIPFLAGS) -o $@ $@.debug

prefs: build/os4/sftphprefs

build/os4/sftphprefs: $(PREFS_SRC) $(AMISSL_OBJ) $(wildcard src/core/*.h src/os4/*.h)
	$(CC) $(CFLAGS) -o $@.debug $(PREFS_SRC) $(AMISSL_OBJ) $(LDFLAGS) $(LIBS)
	$(STRIP) $(STRIPFLAGS) -o $@ $@.debug

nettest: build/os4/nettest

build/os4/nettest: tests/os4/nettest.c
	$(CC) $(CFLAGS) -Wno-error -o $@ $< $(LDFLAGS) -lssh2 -lamisslstubs -lamisslauto
