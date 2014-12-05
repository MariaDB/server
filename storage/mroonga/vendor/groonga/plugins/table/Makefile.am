EXTRA_DIST =					\
	CMakeLists.txt

AM_CFLAGS =					\
	$(MESSAGE_PACK_CFLAGS)			\
	$(MRUBY_CFLAGS)

AM_CPPFLAGS =			\
	-I$(top_builddir)	\
	-I$(top_srcdir)/include	\
	-I$(top_srcdir)/lib

AM_LDFLAGS =					\
	-avoid-version				\
	-module					\
	-no-undefined

LIBS =						\
	$(top_builddir)/lib/libgroonga.la	\
	$(MESSAGE_PACK_LIBS)

table_plugins_LTLIBRARIES = table.la

include sources.am
