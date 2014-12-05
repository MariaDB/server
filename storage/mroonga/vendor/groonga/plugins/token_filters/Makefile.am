EXTRA_DIST =					\
	CMakeLists.txt

AM_CPPFLAGS =					\
	-I$(top_builddir)			\
	-I$(top_srcdir)/include			\
	-I$(top_srcdir)/lib

AM_LDFLAGS =					\
	-avoid-version				\
	-module					\
	-no-undefined

LIBS =						\
	$(top_builddir)/lib/libgroonga.la

token_filter_plugins_LTLIBRARIES =
token_filter_plugins_LTLIBRARIES += stop_word.la

include stop_word_sources.am
