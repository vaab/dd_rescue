# Makefile for dd_rescue
# (c) garloff@suse.de, 99/10/09, GNU GPL
# $Id$

CC = gcc
CFLAGS = -O2 -g
INSTALL = install
INSTALLFLAGS = -s
prefix = /usr
INSTALLDIR = $(prefix)/bin
TARGETS = dd_rescue
OBJECTS = dd_rescue.o

default: $(TARGETS)

dd_rescue: dd_rescue.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f $(TARGETS) $(OBJECTS) core

distclean: clean
	rm -f *~

install: $(TARGETS)
	$(INSTALL) $(INSTALLFLAGS) -o root -g root -m 755 $(TARGETS) $(INSTALLDIR)

