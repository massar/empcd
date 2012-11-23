# /**************************************
#  EventMusicPlayerClient daemon
#  by Jeroen Massar <jeroen@unfix.org>
# ***************************************
# $Author: $
# $Id: $
# $Date: $
# **************************************/
#
# Source Makefile for empcd - Jeroen Massar <jeroen@unfix.org>
#

BINS	= empcd
SRCS	= empcd.c keyeventtable.c support/mpc-0.12.2/src/libmpdclient.c
INCS	= empcd.h
DEPS	= Makefile
OBJS	= empcd.o keyeventtable.o support/mpc-0.12.2/src/libmpdclient.o
WARNS	= -W -Wall -pedantic -Wno-format -Wno-unused -Wno-long-long
EXTRA   = -g3
CFLAGS	= $(WARNS) $(EXTRA) -D_GNU_SOURCE
LDFLAGS	=
CC      = gcc
RM      = rm
DESTDIR	= /
dirsbin = /usr/sbin/
dirdoc  = /usr/share/doc/empcd/

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
	@debian/rules binary

# Mark targets as phony
.PHONY : all clean deb

