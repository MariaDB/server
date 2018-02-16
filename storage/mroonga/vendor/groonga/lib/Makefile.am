SUBDIRS =					\
	dat					\
	mrb					\
	proc					\
	ts

lib_LTLIBRARIES = libgroonga.la

include $(top_srcdir)/version.sh

AM_CPPFLAGS =					\
	$(MRUBY_CPPFLAGS)

AM_CFLAGS =					\
	$(NO_STRICT_ALIASING_CFLAGS)		\
	$(COVERAGE_CFLAGS)			\
	$(GRN_CFLAGS)				\
	$(MESSAGE_PACK_CFLAGS)			\
	$(LIBLZ4_CFLAGS)			\
	$(LIBZSTD_CFLAGS)

AM_CXXFLAGS =					\
	$(NO_STRICT_ALIASING_CFLAGS)		\
	$(COVERAGE_CFLAGS)			\
	$(GRN_CXXFLAGS)				\
	$(ARROW_CFLAGS)

BUNDLED_LIBRARIES_CFLAGS =			\
	$(MRUBY_CFLAGS)				\
	$(ONIGMO_CFLAGS)

DEFAULT_INCLUDES =				\
	-I$(top_builddir)			\
	-I$(top_srcdir)/include			\
	$(BUNDLED_LIBRARIES_CFLAGS)

DEFS += -D_REENTRANT $(GRN_DEFS) -DGRN_DAT_EXPORT

include c_sources.am
include cpp_sources.am
libgroonga_la_SOURCES =				\
	$(libgroonga_c_sources)			\
	$(libgroonga_cpp_source)

#nfkc.c:
#	$(RUBY) nfkc.rb --impl=table

libgroonga_la_LDFLAGS =				\
	-version-info $(LT_VERSION_INFO)	\
	-no-undefined				\
	$(WINDOWS_LDFLAGS)

libgroonga_la_LIBADD =				\
	dat/libgrndat.la			\
	mrb/libgrnmrb.la			\
	proc/libgrnproc.la			\
	ts/libgrnts.la				\
	$(MESSAGE_PACK_LIBS)

if WITH_MRUBY
libgroonga_la_LIBADD +=				\
	../vendor/mruby/libmruby.la
endif

libgroonga_la_LIBADD +=				\
	$(ONIGMO_LIBS)				\
	$(LIBLZ4_LIBS)				\
	$(LIBZSTD_LIBS)				\
	$(ATOMIC_LIBS)				\
	$(ARROW_LIBS)

if WITH_LEMON
BUILT_SOURCES =					\
	grn_ecmascript.c

SUFFIXES = .lemon .c

.lemon.c:
	$(LEMON) $<
endif

if PLATFORM_WIN32
libgroonga_la_SOURCES +=			\
	metadata.rc

.rc.lo:
	$(LIBTOOL) $(AM_V_lt) --tag=RC		\
	  $(AM_LIBTOOLFLAGS) $(LIBTOOLFLAGS)	\
	  --mode=compile			\
	  $(RC) $(RCFLAGS) -o $@ $<
endif

EXTRA_DIST =					\
	grn_ecmascript.c			\
	grn_ecmascript.h			\
	grn_ecmascript.lemon			\
	CMakeLists.txt

CLEANFILES = *.gcno *.gcda
