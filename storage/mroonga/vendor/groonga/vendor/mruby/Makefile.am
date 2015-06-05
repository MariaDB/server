EXTRA_DIST =					\
	build_config.rb				\
	mruby_build.rb				\
	mruby_build.timestamp

DEFAULT_INCLUDES =				\
	-I$(srcdir)/../mruby-source/include	\
	-I$(srcdir)/../mruby-source/src		\
	-Imruby-io/include			\
	-I$(srcdir)/mruby-io/include

CFLAGS += $(NO_FLOAT_EQUAL_CFLAGS)

if WITH_MRUBY
noinst_LTLIBRARIES = libmruby.la

AM_CPPFLAGS =					\
	-I$(srcdir)/../onigmo-source

if PLATFORM_WIN32
AM_CPPFLAGS +=					\
	-DMRB_BUILD_AS_DLL			\
	-DMRB_CORE
endif

include sources.am
include built_sources.am
libmruby_la_SOURCES += $(BUILT_SOURCES)

parse.c: mruby_build.timestamp
mrblib.c: mruby_build.timestamp
mrbgems_init.c: mruby_build.timestamp
mruby-onig-regexp/src/mruby_onig_regexp.c: mruby_build.timestamp
mruby-env/src/env.c: mruby_build.timestamp
mruby-io/include/mruby/ext/io.h: mruby_build.timestamp
mruby-io/src/file.c: mruby_build.timestamp
mruby-io/src/file_test.c: mruby_build.timestamp
mruby-io/src/io.c: mruby_build.timestamp
mruby-io/src/mruby_io_gem.c: mruby_build.timestamp

MRUBY_CONFIG = $(abs_srcdir)/build_config.rb
MRUBY_BUILD_DIR = $(abs_top_builddir)/vendor/mruby-build
mruby_build.timestamp: build_config.rb
	$(RUBY) "$(srcdir)/mruby_build.rb"	\
	  "$(srcdir)/build_config.rb"		\
	  "$(srcdir)/../mruby-source"		\
	  "$(builddir)/../mruby-build"		\
	  "$(srcdir)/../onigmo-source"		\
	  "mruby_build.timestamp"

CLEANFILES = *.gcno *gcda
MAINTAINERCLEANFILES = mruby_build.timestamp
endif

update:
	cd "$(srcdir)/../mruby-source" && \
	  (git checkout master && git pull --rebase)
	cd "$(srcdir)" && \
	  ./update.rb build_config.rb ../mruby-source > sources.am
