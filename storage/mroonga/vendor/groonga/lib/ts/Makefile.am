BUNDLED_LIBRARIES_CFLAGS =			\
	$(MRUBY_CFLAGS)				\
	$(ONIGMO_CFLAGS)

DEFAULT_INCLUDES =				\
	-I$(top_builddir)			\
	-I$(top_srcdir)/include			\
	$(BUNDLED_LIBRARIES_CFLAGS)

AM_CFLAGS =					\
	$(NO_STRICT_ALIASING_CFLAGS)		\
	$(COVERAGE_CFLAGS)			\
	$(GRN_CFLAGS)				\
	$(MESSAGE_PACK_CFLAGS)

noinst_LTLIBRARIES = libgrnts.la

include sources.am

CLEANFILES = *.gcno *.gcda
