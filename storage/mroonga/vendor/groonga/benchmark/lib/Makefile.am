if WITH_BENCHMARK
noinst_LTLIBRARIES =		\
	libbenchmark.la
endif

AM_CPPFLAGS =			\
	-I$(srcdir)

AM_CFLAGS =			\
	$(GLIB_CFLAGS)

CFLAGS +=					\
	$(NO_BAD_FUNCTION_CAST_CFLAGS)

LIBS =				\
	$(GLIB_LIBS)

libbenchmark_la_SOURCES =	\
	benchmark.c		\
	benchmark.h		\
	bench-reporter.c	\
	bench-reporter.h	\
	bench-utils.c		\
	bench-utils.h
