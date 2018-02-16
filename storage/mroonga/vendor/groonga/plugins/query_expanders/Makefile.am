EXTRA_DIST =					\
	CMakeLists.txt

AM_CPPFLAGS =			\
	-I$(top_builddir)	\
	-I$(top_srcdir)/include	\
	-I$(top_srcdir)/lib

AM_LDFLAGS =					\
	-avoid-version				\
	-module					\
	-no-undefined

LIBS =						\
	$(top_builddir)/lib/libgroonga.la

query_expander_plugins_LTLIBRARIES =
query_expander_plugins_LTLIBRARIES += tsv.la

include tsv_sources.am
