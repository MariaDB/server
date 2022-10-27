bin_PROGRAMS =

NONEXISTENT_CXX_SOURCE = nonexistent.cpp

bin_PROGRAMS +=					\
	groonga-suggest-create-dataset

if ENABLE_SUGGEST_LEARNER
bin_PROGRAMS +=					\
	groonga-suggest-learner			\
	groonga-suggest-httpd
noinst_LTLIBRARIES = libutil.la
endif

EXTRA_DIST =					\
	CMakeLists.txt

AM_CFLAGS =					\
	$(NO_STRICT_ALIASING_CFLAGS)		\
	$(COVERAGE_CFLAGS)			\
	$(GRN_CFLAGS)				\
	$(MESSAGE_PACK_CFLAGS)			\
	$(MRUBY_CFLAGS)

DEFS += $(GRN_DEFS)

AM_LDFLAGS = -no-undefined

DEFAULT_INCLUDES =				\
	-I$(top_builddir)			\
	-I$(srcdir)				\
	-I$(top_srcdir)/include			\
	-I$(top_srcdir)/lib			\
	$(GROONGA_INCLUDEDIR)

include learner_sources.am
nodist_EXTRA_groonga_suggest_learner_SOURCES = $(NONEXISTENT_CXX_SOURCE)
groonga_suggest_learner_CFLAGS =		\
	$(AM_CFLAGS)				\
	$(LIBEVENT_CFLAGS)			\
	$(LIBZMQ_CFLAGS)			\
	$(MESSAGE_PACK_CFLAGS)
groonga_suggest_learner_LDADD =			\
	libutil.la				\
	$(top_builddir)/lib/libgroonga.la	\
	$(LIBEVENT_LIBS)			\
	$(LIBZMQ_LIBS)				\
	$(MESSAGE_PACK_LIBS)

include httpd_sources.am
nodist_EXTRA_groonga_suggest_httpd_SOURCES = $(NONEXISTENT_CXX_SOURCE)
groonga_suggest_httpd_CFLAGS =			\
	$(AM_CFLAGS)				\
	$(LIBEVENT_CFLAGS)			\
	$(LIBZMQ_CFLAGS)			\
	$(MESSAGE_PACK_CFLAGS)
groonga_suggest_httpd_LDADD =			\
	libutil.la				\
	$(top_builddir)/lib/libgroonga.la	\
	$(LIBEVENT_LIBS)			\
	$(LIBZMQ_LIBS)				\
	$(MESSAGE_PACK_LIBS)

include create_dataset_sources.am
nodist_EXTRA_groonga_suggest_create_dataset_SOURCES = $(NONEXISTENT_CXX_SOURCE)
groonga_suggest_create_dataset_CFLAGS =		\
	$(AM_CFLAGS)
groonga_suggest_create_dataset_LDADD =		\
	$(top_builddir)/lib/libgroonga.la

include util_sources.am
libutil_la_CFLAGS =				\
	$(AM_CFLAGS)				\
	$(LIBEVENT_CFLAGS)
