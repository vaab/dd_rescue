# Makefile for dd_rescue
# (c) garloff@suse.de, 99/10/09, GNU GPL
# $Id$

VERSION = 0.95

CC = gcc
CFLAGS = -O2 -Wall -g $(EXTRA_CFLAGS)
DEFINES = -DVERSION=\"$(VERSION)\"
INSTALL = install
INSTALLFLAGS = -s
prefix = /usr
#INSTALLDIR = $(prefix)/bin
INSTALLDIR = /bin
#MYDIR = dd_rescue-$(VERSION)
MYDIR = dd_rescue
TARGETS = dd_rescue
OBJECTS = dd_rescue.o

default: $(TARGETS)

dd_rescue: dd_rescue.c
	$(CC) $(CFLAGS) $(DEFINES) $< -o $@

clean:
	rm -f $(TARGETS) $(OBJECTS) core

distclean: clean
	rm -f *~

dist: distclean
	tar cvzf ../dd_rescue-$(VERSION).tar.gz -C.. --exclude=$(MYDIR)/CV* $(MYDIR)/

install: $(TARGETS)
	$(INSTALL) $(INSTALLFLAGS) -o root -g root -m 755 $(TARGETS) $(INSTALLDIR)

