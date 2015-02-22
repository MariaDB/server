EXTRA_DIST =					\
	build_config.rb				\
	mruby-build.timestamp

DEFAULT_INCLUDES =				\
	-I$(srcdir)/../mruby-source/include	\
	-I$(srcdir)/../mruby-source/src		\
	-Imruby-io/include			\
	-I$(srcdir)/mruby-io/include

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

BUILT_SOURCES =					\
	parse.c					\
	mrblib.c				\
	mrbgems_init.c
BUILT_SOURCES +=				\
	mruby-onig-regexp/src/mruby_onig_regexp.c
BUILT_SOURCES +=				\
	mruby-io/include/mruby/ext/io.h		\
	mruby-io/src/file.c			\
	mruby-io/src/file_test.c		\
	mruby-io/src/io.c			\
	mruby-io/src/mruby_io_gem.c
libmruby_la_SOURCES += $(BUILT_SOURCES)

parse.c: mruby-build.timestamp
mrblib.c: mruby-build.timestamp
mrbgems_init.c: mruby-build.timestamp
mruby-onig-regexp/src/mruby_onig_regexp.c: mruby-build.timestamp
mruby-io/include/mruby/ext/io.h: mruby-build.timestamp
mruby-io/src/file.c: mruby-build.timestamp
mruby-io/src/file_test.c: mruby-build.timestamp
mruby-io/src/io.c: mruby-build.timestamp
mruby-io/src/mruby_io_gem.c: mruby-build.timestamp

MRUBY_CONFIG = $(abs_srcdir)/build_config.rb
MRUBY_BUILD_DIR = $(abs_top_builddir)/vendor/mruby-build
MRUBY_ONIG_REGEXP_DIR = $(MRUBY_BUILD_DIR)/mrbgems/mruby-onig-regexp
MRUBY_IO_DIR = $(MRUBY_BUILD_DIR)/mrbgems/mruby-io
mruby-build.timestamp: build_config.rb
	rm -rf $(MRUBY_BUILD_DIR)
	cd $(srcdir)/../mruby-source &&		\
	  $(RUBY) minirake			\
	    MRUBY_BUILD_DIR=$(MRUBY_BUILD_DIR)	\
	    MRUBY_CONFIG=$(MRUBY_CONFIG)
	touch $@
	cp $(MRUBY_BUILD_DIR)/host/src/y.tab.c parse.c
	cp $(MRUBY_BUILD_DIR)/host/mrblib/mrblib.c ./
	(							\
	  cat $(MRUBY_BUILD_DIR)/host/mrbgems/gem_init.c;	\
	  cat $(MRUBY_BUILD_DIR)/host/mrbgems/*/gem_init.c;	\
	) > mrbgems_init.c
	$(MKDIR_P) mruby-onig-regexp/
	cp -r $(MRUBY_ONIG_REGEXP_DIR)/src/ mruby-onig-regexp/
	$(MKDIR_P) mruby-io/
	cp -r $(MRUBY_IO_DIR)/include/ mruby-io/
	cp -r $(MRUBY_IO_DIR)/src/ mruby-io/

EXTRA_DIST +=					\
	mruby-build.timestamp

CLEANFILES = *.gcno *gcda
MAINTAINERCLEANFILES = mruby-build.timestamp
endif

update:
	cd "$(srcdir)/../mruby-source" && \
	  (git checkout master && git pull --rebase)
	cd "$(srcdir)" && \
	  ./update.rb build_config.rb ../mruby-source > sources.am
