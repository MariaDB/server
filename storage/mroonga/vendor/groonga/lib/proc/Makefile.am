AM_CPPFLAGS =					\
	-I$(top_builddir)			\
	-I$(top_srcdir)/include			\
	-I$(top_srcdir)/lib

AM_CFLAGS =					\
	$(NO_STRICT_ALIASING_CFLAGS)		\
	$(COVERAGE_CFLAGS)			\
	$(GRN_CFLAGS)				\
	$(MESSAGE_PACK_CFLAGS)			\
	$(MRUBY_CFLAGS)

noinst_LTLIBRARIES = libgrnproc.la

include sources.am

CLEANFILES = *.gcno *.gcda
