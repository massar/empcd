# /**************************************
#  EventMusicPlayerClient daemon (empcd)
#  by Jeroen Massar <jeroen@massar.ch>
# ***************************************
# $Author: $
# $Id: $
# $Date: $
# **************************************/
#
# Source Makefile for empcd
#

BINS	= empcd
SRCS	= empcd.c keyeventtable.c support/mpc-0.12.2/src/libmpdclient.c
INCS	= empcd.h
DEPS	= Makefile
OBJS	= empcd.o keyeventtable.o support/mpc-0.12.2/src/libmpdclient.o
WARNS	= -W -Wall -pedantic -Wno-format -Wno-unused -Wno-long-long
EXTRA   = -g3
CFLAGS	+= $(WARNS) $(EXTRA)
LDFLAGS	+=
CC      = gcc
RM      = rm
DESTDIR	= /
dirsbin = /usr/sbin/
dirdoc  = /usr/share/doc/empcd/

# Our very *bliep* set of options to make sure that these things can't cause any issues
CFLAGS += -W -Wall -Wshadow -Wpointer-arith -Wwrite-strings
CFLAGS += -Waggregate-return -Wstrict-prototypes -Wmissing-prototypes
CFLAGS += -Wmissing-declarations -Wredundant-decls -Wnested-externs
CFLAGS += -Winline -Wbad-function-cast -Wunused -Winit-self -Wextra
CFLAGS += -Wno-long-long -Wmissing-include-dirs
CFLAGS += -Wno-packed -pedantic -Wno-variadic-macros -Wswitch-default
CFLAGS += -Wformat=2 -Wformat-security -Wmissing-format-attribute
CFLAGS += -fshort-enums -fstrict-aliasing -fno-common
CFLAGS += -D_REENTRANT -D_THREAD_SAFE -pipe

# Export some things
export DESTDIR
export dirsbin
export dirdoc

# Make Targets
all:	$(BINS)

empcd:	$(OBJS) ${INCS} ${DEPS}
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

clean:
	$(RM) -rf $(OBJS) $(BINS) build-stamp configure-stamp debian/*.debhelper debian/empcd.substvars debian/files debian/dirs debian/empcd

distclean: clean

# Install the program into ${DESTDIR}
# RPM's don't want the docs so it won't get it ;)
install: all
	@echo "Installing into ${DESTDIR}..."
	@echo "Binaries..."
	@mkdir -p ${DESTDIR}${dirsbin}
	@cp empcd ${DESTDIR}${dirsbin}
	@mkdir -p ${DESTDIR}${dirdoc}
	@cp README.md ${DESTDIR}${dirdoc}
	@echo "Configuration..."
	@mkdir -p ${DESTDIR}${diretc}
	@echo "Installation into ${DESTDIR}/ completed"

deb:
	@debuild -us -uc

# Mark targets as phony
.PHONY : all clean deb

