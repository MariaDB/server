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

function_plugins_LTLIBRARIES =
function_plugins_LTLIBRARIES += vector.la

include vector_sources.am
