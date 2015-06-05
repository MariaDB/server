SUBDIRS =					\
	dat					\
	mrb

lib_LTLIBRARIES = libgroonga.la

include $(top_srcdir)/version.sh
AM_CFLAGS =					\
	$(NO_STRICT_ALIASING_CFLAGS)		\
	$(COVERAGE_CFLAGS)			\
	$(GRN_CFLAGS)				\
	$(MESSAGE_PACK_CFLAGS)			\
	$(LIBLZ4_CFLAGS)

BUNDLED_LIBRARIES_CFLAGS =			\
	$(MRUBY_CFLAGS)				\
	$(ONIGMO_CFLAGS)

DEFAULT_INCLUDES =				\
	-I$(top_builddir)			\
	-I$(top_srcdir)/include			\
	$(BUNDLED_LIBRARIES_CFLAGS)

DEFS += -D_REENTRANT $(GRN_DEFS) -DGRN_DAT_EXPORT

include sources.am

libgroonga_la_LDFLAGS =				\
	-version-info $(LT_VERSION_INFO)	\
	-no-undefined				\
	$(WINDOWS_LDFLAGS)

libgroonga_la_LIBADD =				\
	dat/libgrndat.la			\
	mrb/libgrnmrb.la			\
	$(MESSAGE_PACK_LIBS)

if WITH_MRUBY
libgroonga_la_LIBADD +=				\
	../vendor/mruby/libmruby.la
endif

libgroonga_la_LIBADD +=				\
	$(ONIGMO_LIBS)				\
	$(LIBLZ4_LIBS)

if WITH_LEMON
BUILT_SOURCES =					\
	grn_ecmascript.c

SUFFIXES = .lemon .c

.lemon.c:
	$(LEMON) $<
endif

EXTRA_DIST =					\
	grn_ecmascript.c			\
	grn_ecmascript.h			\
	grn_ecmascript.lemon			\
	CMakeLists.txt

CLEANFILES = *.gcno *.gcda
