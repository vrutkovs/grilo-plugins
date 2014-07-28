#
# Makefile.am
#
# Author: Vadim Rutkovsky <vrutkovs@redhat.com>
#
# Copyright (C) 2014 Red Hat

include $(top_srcdir)/gtester.mk

ext_LTLIBRARIES = libgrlampache.la

libgrlampache_la_CFLAGS =	\
	$(DEPS_AMPACHE_CFLAGS)	\
	-DG_LOG_DOMAIN=\"grlampache\" \
	-DLOCALEDIR=\"$(localedir)\"

libgrlampache_la_LIBADD =	\
	$(DEPS_AMPACHE_LIBS)

libgrlampache_la_LDFLAGS = \
	-no-undefined	  \
	-module		  \
	-avoid-version

libgrlampache_la_SOURCES = 		\
	grl-ampache.c			\
	grl-ampache.h

extdir 			= $(GRL_PLUGINS_DIR)
ampachexmldir 	= $(GRL_PLUGINS_DIR)
ampachexml_DATA	= $(AMPACHE_PLUGIN_ID).xml

# This lets us test the plugin without installing it,
# because grilo expects the .so and .xml files to be in
# the same directory:
copy-xml-to-libs-dir: libgrlampache.la
	cp -f $(srcdir)/$(ampachexml_DATA) $(builddir)/.libs/

all-local: copy-xml-to-libs-dir

EXTRA_DIST = $(ampachexml_DATA)

MAINTAINERCLEANFILES =	\
	*.in		\
	*~

DISTCLEANFILES = $(MAINTAINERCLEANFILES)
