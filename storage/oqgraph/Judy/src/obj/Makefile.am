AM_CFLAGS = @WARN_CFLAGS@

CLEANFILES = libJudy.la

include_HEADERS = $(srcdir)/../Judy.h

lib_LTLIBRARIES = libJudy.la

libJudy_la_SOURCES =

libJudy_la_LIBADD = $(builddir)/../JudyCommon/*.lo $(builddir)/../JudyL/*.lo $(builddir)/../Judy1/*.lo $(builddir)/../JudyHS/*.lo $(builddir)/../JudySL/*.lo 

libJudy_la_LDFLAGS = @VERSION_INFO@
