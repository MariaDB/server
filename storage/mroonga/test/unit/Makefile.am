if WITH_CUTTER
noinst_LTLIBRARIES =				\
	test_mrn_path_mapper.la
endif

AM_CPPFLAGS =					\
	$(GROONGA_CFLAGS)			\
	$(CPPCUTTER_CFLAGS)			\
	-I$(top_srcdir)				\
	-I$(top_srcdir)/lib

AM_LDFLAGS =					\
	-module					\
	-rpath $(libdir)			\
	-avoid-version				\
	-no-undefined

LIBS =						\
	$(CPPCUTTER_LIBS)			\
	$(GROONGA_LIBS)				\
	$(MECAB_LIBS)

test_mrn_path_mapper_la_SOURCES =		\
	test_mrn_path_mapper.cpp

test_mrn_path_mapper_la_LIBADD =		\
	$(top_builddir)/lib/libmrn_no_mysql.la
