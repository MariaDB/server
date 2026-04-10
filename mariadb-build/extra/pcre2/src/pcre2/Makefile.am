## Process this file with automake to produce Makefile.in.

AUTOMAKE_OPTIONS = subdir-objects
ACLOCAL_AMFLAGS = -I m4

## This seems to have become necessary for building in non-source directory.

AM_CPPFLAGS="-I$(srcdir)/src"

## Set some variables to accumulate conditionally-set Automake targets.

CLEAN_LOCAL_TARGETS=
DISTCLEAN_LOCAL_TARGETS=
PHONY_TARGETS=

## Specify the documentation files that are distributed.

dist_doc_DATA = \
  AUTHORS.md \
  COPYING \
  ChangeLog \
  LICENCE.md \
  NEWS \
  README \
  SECURITY.md \
  doc/pcre2.txt \
  doc/pcre2-config.txt \
  doc/pcre2grep.txt \
  doc/pcre2test.txt

dist_html_DATA = \
  doc/html/NON-AUTOTOOLS-BUILD.txt \
  doc/html/README.txt \
  doc/html/index.html \
  doc/html/pcre2-config.html \
  doc/html/pcre2.html \
  doc/html/pcre2_callout_enumerate.html \
  doc/html/pcre2_code_copy.html \
  doc/html/pcre2_code_copy_with_tables.html \
  doc/html/pcre2_code_free.html \
  doc/html/pcre2_compile.html \
  doc/html/pcre2_compile_context_copy.html \
  doc/html/pcre2_compile_context_create.html \
  doc/html/pcre2_compile_context_free.html \
  doc/html/pcre2_config.html \
  doc/html/pcre2_convert_context_copy.html \
  doc/html/pcre2_convert_context_create.html \
  doc/html/pcre2_convert_context_free.html \
  doc/html/pcre2_converted_pattern_free.html \
  doc/html/pcre2_dfa_match.html \
  doc/html/pcre2_general_context_copy.html \
  doc/html/pcre2_general_context_create.html \
  doc/html/pcre2_general_context_free.html \
  doc/html/pcre2_get_error_message.html \
  doc/html/pcre2_get_mark.html \
  doc/html/pcre2_get_match_data_heapframes_size.html \
  doc/html/pcre2_get_match_data_size.html \
  doc/html/pcre2_get_ovector_count.html \
  doc/html/pcre2_get_ovector_pointer.html \
  doc/html/pcre2_get_startchar.html \
  doc/html/pcre2_jit_compile.html \
  doc/html/pcre2_jit_free_unused_memory.html \
  doc/html/pcre2_jit_match.html \
  doc/html/pcre2_jit_stack_assign.html \
  doc/html/pcre2_jit_stack_create.html \
  doc/html/pcre2_jit_stack_free.html \
  doc/html/pcre2_maketables.html \
  doc/html/pcre2_maketables_free.html \
  doc/html/pcre2_match.html \
  doc/html/pcre2_match_context_copy.html \
  doc/html/pcre2_match_context_create.html \
  doc/html/pcre2_match_context_free.html \
  doc/html/pcre2_match_data_create.html \
  doc/html/pcre2_match_data_create_from_pattern.html \
  doc/html/pcre2_match_data_free.html \
  doc/html/pcre2_next_match.html \
  doc/html/pcre2_pattern_convert.html \
  doc/html/pcre2_pattern_info.html \
  doc/html/pcre2_serialize_decode.html \
  doc/html/pcre2_serialize_encode.html \
  doc/html/pcre2_serialize_free.html \
  doc/html/pcre2_serialize_get_number_of_codes.html \
  doc/html/pcre2_set_bsr.html \
  doc/html/pcre2_set_callout.html \
  doc/html/pcre2_set_character_tables.html \
  doc/html/pcre2_set_compile_extra_options.html \
  doc/html/pcre2_set_compile_recursion_guard.html \
  doc/html/pcre2_set_depth_limit.html \
  doc/html/pcre2_set_glob_escape.html \
  doc/html/pcre2_set_glob_separator.html \
  doc/html/pcre2_set_heap_limit.html \
  doc/html/pcre2_set_match_limit.html \
  doc/html/pcre2_set_max_pattern_compiled_length.html \
  doc/html/pcre2_set_max_pattern_length.html \
  doc/html/pcre2_set_max_varlookbehind.html \
  doc/html/pcre2_set_offset_limit.html \
  doc/html/pcre2_set_optimize.html \
  doc/html/pcre2_set_newline.html \
  doc/html/pcre2_set_parens_nest_limit.html \
  doc/html/pcre2_set_recursion_limit.html \
  doc/html/pcre2_set_recursion_memory_management.html \
  doc/html/pcre2_set_substitute_callout.html \
  doc/html/pcre2_set_substitute_case_callout.html \
  doc/html/pcre2_substitute.html \
  doc/html/pcre2_substring_copy_byname.html \
  doc/html/pcre2_substring_copy_bynumber.html \
  doc/html/pcre2_substring_free.html \
  doc/html/pcre2_substring_get_byname.html \
  doc/html/pcre2_substring_get_bynumber.html \
  doc/html/pcre2_substring_length_byname.html \
  doc/html/pcre2_substring_length_bynumber.html \
  doc/html/pcre2_substring_list_free.html \
  doc/html/pcre2_substring_list_get.html \
  doc/html/pcre2_substring_nametable_scan.html \
  doc/html/pcre2_substring_number_from_name.html \
  doc/html/pcre2api.html \
  doc/html/pcre2build.html \
  doc/html/pcre2callout.html \
  doc/html/pcre2compat.html \
  doc/html/pcre2convert.html \
  doc/html/pcre2demo.html \
  doc/html/pcre2grep.html \
  doc/html/pcre2jit.html \
  doc/html/pcre2limits.html \
  doc/html/pcre2matching.html \
  doc/html/pcre2partial.html \
  doc/html/pcre2pattern.html \
  doc/html/pcre2perform.html \
  doc/html/pcre2posix.html \
  doc/html/pcre2sample.html \
  doc/html/pcre2serialize.html \
  doc/html/pcre2syntax.html \
  doc/html/pcre2test.html \
  doc/html/pcre2unicode.html

dist_man_MANS = \
  doc/pcre2-config.1 \
  doc/pcre2.3 \
  doc/pcre2_callout_enumerate.3 \
  doc/pcre2_code_copy.3 \
  doc/pcre2_code_copy_with_tables.3 \
  doc/pcre2_code_free.3 \
  doc/pcre2_compile.3 \
  doc/pcre2_compile_context_copy.3 \
  doc/pcre2_compile_context_create.3 \
  doc/pcre2_compile_context_free.3 \
  doc/pcre2_config.3 \
  doc/pcre2_convert_context_copy.3 \
  doc/pcre2_convert_context_create.3 \
  doc/pcre2_convert_context_free.3 \
  doc/pcre2_converted_pattern_free.3 \
  doc/pcre2_dfa_match.3 \
  doc/pcre2_general_context_copy.3 \
  doc/pcre2_general_context_create.3 \
  doc/pcre2_general_context_free.3 \
  doc/pcre2_get_error_message.3 \
  doc/pcre2_get_mark.3 \
  doc/pcre2_get_match_data_heapframes_size.3 \
  doc/pcre2_get_match_data_size.3 \
  doc/pcre2_get_ovector_count.3 \
  doc/pcre2_get_ovector_pointer.3 \
  doc/pcre2_get_startchar.3 \
  doc/pcre2_jit_compile.3 \
  doc/pcre2_jit_free_unused_memory.3 \
  doc/pcre2_jit_match.3 \
  doc/pcre2_jit_stack_assign.3 \
  doc/pcre2_jit_stack_create.3 \
  doc/pcre2_jit_stack_free.3 \
  doc/pcre2_maketables.3 \
  doc/pcre2_maketables_free.3 \
  doc/pcre2_match.3 \
  doc/pcre2_match_context_copy.3 \
  doc/pcre2_match_context_create.3 \
  doc/pcre2_match_context_free.3 \
  doc/pcre2_match_data_create.3 \
  doc/pcre2_match_data_create_from_pattern.3 \
  doc/pcre2_match_data_free.3 \
  doc/pcre2_next_match.3 \
  doc/pcre2_pattern_convert.3 \
  doc/pcre2_pattern_info.3 \
  doc/pcre2_serialize_decode.3 \
  doc/pcre2_serialize_encode.3 \
  doc/pcre2_serialize_free.3 \
  doc/pcre2_serialize_get_number_of_codes.3 \
  doc/pcre2_set_bsr.3 \
  doc/pcre2_set_callout.3 \
  doc/pcre2_set_character_tables.3 \
  doc/pcre2_set_compile_extra_options.3 \
  doc/pcre2_set_compile_recursion_guard.3 \
  doc/pcre2_set_depth_limit.3 \
  doc/pcre2_set_glob_escape.3 \
  doc/pcre2_set_glob_separator.3 \
  doc/pcre2_set_heap_limit.3 \
  doc/pcre2_set_match_limit.3 \
  doc/pcre2_set_max_pattern_compiled_length.3 \
  doc/pcre2_set_max_pattern_length.3 \
  doc/pcre2_set_max_varlookbehind.3 \
  doc/pcre2_set_offset_limit.3 \
  doc/pcre2_set_optimize.3 \
  doc/pcre2_set_newline.3 \
  doc/pcre2_set_parens_nest_limit.3 \
  doc/pcre2_set_recursion_limit.3 \
  doc/pcre2_set_recursion_memory_management.3 \
  doc/pcre2_set_substitute_callout.3 \
  doc/pcre2_set_substitute_case_callout.3 \
  doc/pcre2_substitute.3 \
  doc/pcre2_substring_copy_byname.3 \
  doc/pcre2_substring_copy_bynumber.3 \
  doc/pcre2_substring_free.3 \
  doc/pcre2_substring_get_byname.3 \
  doc/pcre2_substring_get_bynumber.3 \
  doc/pcre2_substring_length_byname.3 \
  doc/pcre2_substring_length_bynumber.3 \
  doc/pcre2_substring_list_free.3 \
  doc/pcre2_substring_list_get.3 \
  doc/pcre2_substring_nametable_scan.3 \
  doc/pcre2_substring_number_from_name.3 \
  doc/pcre2api.3 \
  doc/pcre2build.3 \
  doc/pcre2callout.3 \
  doc/pcre2compat.3 \
  doc/pcre2convert.3 \
  doc/pcre2demo.3 \
  doc/pcre2grep.1 \
  doc/pcre2jit.3 \
  doc/pcre2limits.3 \
  doc/pcre2matching.3 \
  doc/pcre2partial.3 \
  doc/pcre2pattern.3 \
  doc/pcre2perform.3 \
  doc/pcre2posix.3 \
  doc/pcre2sample.3 \
  doc/pcre2serialize.3 \
  doc/pcre2syntax.3 \
  doc/pcre2test.1 \
  doc/pcre2unicode.3

# The Libtool libraries to install.  We'll add to this later.

lib_LTLIBRARIES =

# Unit tests you want to run when people type 'make check'.
# TESTS is for binary unit tests, check_SCRIPTS for script-based tests

TESTS =
XFAIL_TESTS =
check_SCRIPTS =
dist_noinst_SCRIPTS =

# Some of the binaries we make are to be installed, and others are
# (non-user-visible) helper programs needed to build the libraries.

bin_PROGRAMS =
noinst_PROGRAMS =

# Additional files to delete on 'make clean', 'make distclean',
# and 'make maintainer-clean'. It turns out that the default is to delete only
# those binaries that *this* configuration has created. If the configuration
# has been changed, some binaries may not get automatically deleted. Therefore
# we list them here.

CLEANFILES = \
  pcre2_dftables \
  pcre2_jit_test \
  pcre2fuzzcheck-8 \
  pcre2fuzzcheck-16 \
  pcre2fuzzcheck-32 \
  pcre2demo

DISTCLEANFILES = src/config.h.in~
MAINTAINERCLEANFILES =

# Additional files to bundle with the distribution, over and above what
# the Autotools include by default.

EXTRA_DIST =

# These files contain additional m4 macros that are used by autoconf.

EXTRA_DIST += \
  m4/ax_check_vscript.m4 \
  m4/ax_pthread.m4 \
  m4/pcre2_visibility.m4 \
  m4/pcre2_zos.m4

# These files contain maintenance information

EXTRA_DIST += \
  NON-AUTOTOOLS-BUILD \
  HACKING

# These are support files for building with Bazel or Zig

EXTRA_DIST += \
  BUILD.bazel \
  MODULE.bazel \
  build.zig

# These are support files for building under VMS

EXTRA_DIST += \
  vms/configure.com \
  vms/openvms_readme.txt \
  vms/pcre2.h_patch \
  vms/stdint.h

# These files are usable versions of pcre2.h and config.h that are distributed
# for the benefit of people who are building PCRE2 manually, without the
# Autotools support.

EXTRA_DIST += \
  src/pcre2.h.generic \
  src/config.h.generic

# The only difference between pcre2.h.in and pcre2.h is the setting of the PCRE
# version number. Therefore, we can create the generic version just by copying.

src/pcre2.h.generic: src/pcre2.h.in configure.ac
	rm -f $@
	cp -p src/pcre2.h $@

# It is more complicated for config.h.generic. We need the version that results
# from a default configuration so as to get all the default values for PCRE
# configuration macros such as MATCH_LIMIT and NEWLINE. We can get this by
# doing a configure in a temporary directory. However, some trickery is needed,
# because the source directory may already be configured. If you just try
# running configure in a new directory, it complains. For this reason, we move
# config.status out of the way while doing the default configuration. The
# resulting config.h is munged by perl to put #ifdefs round any #defines for
# macros with values, and to #undef all boolean macros such as HAVE_xxx,
# SUPPORT_xxx, and PCRE2_STATIC/PCRE2POSIX_SHARED. We also get rid of any
# GCC-specific visibility settings.

src/config.h.generic: configure.ac
	rm -rf $@ _generic
	mkdir _generic
	cs=$(srcdir)/config.status; test ! -f $$cs || mv -f $$cs $$cs.aside
	cd _generic && $(abs_top_srcdir)/configure || :
	cs=$(srcdir)/config.status; test ! -f $$cs.aside || mv -f $$cs.aside $$cs
	test -f _generic/src/config.h
	perl -n \
		-e 'BEGIN{$$blank=0;}' \
		-e 'if(/(.+?)\s*__attribute__ \(\(visibility/){print"$$1\n";$$blank=0;next;}' \
		-e 'if(/LT_OBJDIR/){print"/* This is ignored unless you are using libtool. */\n";}' \
		-e 'if(/^#define\s((?:HAVE|SUPPORT|STDC)_\w+|PCRE2_STATIC|PCRE2POSIX_SHARED)/){print"/* #undef $$1 */\n";$$blank=0;next;}' \
		-e 'if(/^#define\s(?!PACKAGE|VERSION)(\w+)/){print"#ifndef $$1\n$$_#endif\n";$$blank=0;next;}' \
		-e 'if(/^\s*$$/){print unless $$blank; $$blank=1;} else{print;$$blank=0;}' \
		_generic/src/config.h >$@
	rm -rf _generic

MAINTAINERCLEANFILES += src/pcre2.h.generic src/config.h.generic

# These are the header files we'll install. We do not distribute pcre2.h
# because it is generated from pcre2.h.in.

nodist_include_HEADERS = src/pcre2.h
include_HEADERS = src/pcre2posix.h

# This is the "config" script.

bin_SCRIPTS = pcre2-config

## ---------------------------------------------------------------
## The pcre2_dftables program is used to rebuild character tables before
## compiling PCRE2, if --enable-rebuild-chartables is specified. It is not an
## installed program. The default (when --enable-rebuild-chartables is not
## specified) is to copy a distributed set of tables that are defined for ASCII
## code. In this case, pcre2_dftables is not needed.

if WITH_REBUILD_CHARTABLES
noinst_PROGRAMS += pcre2_dftables
pcre2_dftables_SOURCES = src/pcre2_dftables.c
src/pcre2_chartables.c: pcre2_dftables$(EXEEXT)
	rm -f $@
	./pcre2_dftables$(EXEEXT) $@
else
if WITH_EBCDIC
if WITH_EBCDIC_NL25
src/pcre2_chartables.c: $(srcdir)/src/pcre2_chartables.c.ebcdic-1047-nl25
	rm -f $@
	$(LN_S) $(abs_srcdir)/src/pcre2_chartables.c.ebcdic-1047-nl25 $(abs_builddir)/src/pcre2_chartables.c
else # WITH_EBCDIC_NL25
src/pcre2_chartables.c: $(srcdir)/src/pcre2_chartables.c.ebcdic-1047-nl15
	rm -f $@
	$(LN_S) $(abs_srcdir)/src/pcre2_chartables.c.ebcdic-1047-nl15 $(abs_builddir)/src/pcre2_chartables.c
endif # WITH_EBCDIC_NL25
else # WITH_EBCDIC
src/pcre2_chartables.c: $(srcdir)/src/pcre2_chartables.c.dist
	rm -f $@
	$(LN_S) $(abs_srcdir)/src/pcre2_chartables.c.dist $(abs_builddir)/src/pcre2_chartables.c
endif # WITH_EBCDIC
endif # WITH_REBUILD_CHARTABLES

BUILT_SOURCES = src/pcre2_chartables.c
NODIST_SOURCES = src/pcre2_chartables.c

## Define the list of common sources, then arrange to build whichever of the
## 8-, 16-, or 32-bit libraries are configured.

COMMON_SOURCES = \
  src/pcre2_auto_possess.c \
  src/pcre2_chkdint.c \
  src/pcre2_compile.c \
  src/pcre2_compile.h \
  src/pcre2_compile_cgroup.c \
  src/pcre2_compile_class.c \
  src/pcre2_config.c \
  src/pcre2_context.c \
  src/pcre2_convert.c \
  src/pcre2_dfa_match.c \
  src/pcre2_error.c \
  src/pcre2_extuni.c \
  src/pcre2_find_bracket.c \
  src/pcre2_internal.h \
  src/pcre2_intmodedep.h \
  src/pcre2_jit_char_inc.h \
  src/pcre2_jit_compile.c \
  src/pcre2_jit_match_inc.h \
  src/pcre2_jit_misc_inc.h \
  src/pcre2_jit_simd_inc.h \
  src/pcre2_maketables.c \
  src/pcre2_match.c \
  src/pcre2_match_data.c \
  src/pcre2_match_next.c \
  src/pcre2_newline.c \
  src/pcre2_ord2utf.c \
  src/pcre2_pattern_info.c \
  src/pcre2_printint_inc.h \
  src/pcre2_script_run.c \
  src/pcre2_serialize.c \
  src/pcre2_string_utils.c \
  src/pcre2_study.c \
  src/pcre2_substitute.c \
  src/pcre2_substring.c \
  src/pcre2_tables.c \
  src/pcre2_ucd.c \
  src/pcre2_ucp.h \
  src/pcre2_ucptables_inc.h \
  src/pcre2_util.h \
  src/pcre2_valid_utf.c \
  src/pcre2_xclass.c

if WITH_PCRE2_8
lib_LTLIBRARIES += libpcre2-8.la
libpcre2_8_la_SOURCES = \
  $(COMMON_SOURCES)
nodist_libpcre2_8_la_SOURCES = \
  $(NODIST_SOURCES)
libpcre2_8_la_CFLAGS = \
  -DPCRE2_CODE_UNIT_WIDTH=8 \
  $(VISIBILITY_CFLAGS) \
  $(CET_CFLAGS) \
  $(AM_CFLAGS)
libpcre2_8_la_LIBADD =
endif # WITH_PCRE2_8

if WITH_PCRE2_16
lib_LTLIBRARIES += libpcre2-16.la
libpcre2_16_la_SOURCES = \
  $(COMMON_SOURCES)
nodist_libpcre2_16_la_SOURCES = \
  $(NODIST_SOURCES)
libpcre2_16_la_CFLAGS = \
  -DPCRE2_CODE_UNIT_WIDTH=16 \
  $(VISIBILITY_CFLAGS) \
  $(CET_CFLAGS) \
  $(AM_CFLAGS)
libpcre2_16_la_LIBADD =
endif # WITH_PCRE2_16

if WITH_PCRE2_32
lib_LTLIBRARIES += libpcre2-32.la
libpcre2_32_la_SOURCES = \
  $(COMMON_SOURCES)
nodist_libpcre2_32_la_SOURCES = \
  $(NODIST_SOURCES)
libpcre2_32_la_CFLAGS = \
  -DPCRE2_CODE_UNIT_WIDTH=32 \
  $(VISIBILITY_CFLAGS) \
  $(CET_CFLAGS) \
  $(AM_CFLAGS)
libpcre2_32_la_LIBADD =
endif # WITH_PCRE2_32

# The pcre2_chartables.c.dist file is the default version of
# pcre2_chartables.c, used unless --enable-rebuild-chartables is specified.

EXTRA_DIST += \
  src/pcre2_chartables.c.dist \
  src/pcre2_chartables.c.ebcdic-1047-nl15 \
  src/pcre2_chartables.c.ebcdic-1047-nl25
CLEANFILES += src/pcre2_chartables.c

# The JIT compiler lives in a separate directory, but its files are #included
# when pcre2_jit_compile.c is processed, so they must be distributed.

EXTRA_DIST += \
  deps/sljit/LICENSE \
  deps/sljit/README.md \
  deps/sljit/sljit_src/sljitConfig.h \
  deps/sljit/sljit_src/sljitConfigCPU.h \
  deps/sljit/sljit_src/sljitConfigInternal.h \
  deps/sljit/sljit_src/sljitLir.c \
  deps/sljit/sljit_src/sljitLir.h \
  deps/sljit/sljit_src/sljitNativeARM_32.c \
  deps/sljit/sljit_src/sljitNativeARM_64.c \
  deps/sljit/sljit_src/sljitNativeARM_T2_32.c \
  deps/sljit/sljit_src/sljitNativeLOONGARCH_64.c \
  deps/sljit/sljit_src/sljitNativeMIPS_32.c \
  deps/sljit/sljit_src/sljitNativeMIPS_64.c \
  deps/sljit/sljit_src/sljitNativeMIPS_common.c \
  deps/sljit/sljit_src/sljitNativePPC_32.c \
  deps/sljit/sljit_src/sljitNativePPC_64.c \
  deps/sljit/sljit_src/sljitNativePPC_common.c \
  deps/sljit/sljit_src/sljitNativeRISCV_32.c \
  deps/sljit/sljit_src/sljitNativeRISCV_64.c \
  deps/sljit/sljit_src/sljitNativeRISCV_common.c \
  deps/sljit/sljit_src/sljitNativeS390X.c \
  deps/sljit/sljit_src/sljitNativeX86_32.c \
  deps/sljit/sljit_src/sljitNativeX86_64.c \
  deps/sljit/sljit_src/sljitNativeX86_common.c \
  deps/sljit/sljit_src/sljitSerialize.c \
  deps/sljit/sljit_src/sljitUtils.c \
  deps/sljit/sljit_src/allocator_src/sljitExecAllocatorApple.c \
  deps/sljit/sljit_src/allocator_src/sljitExecAllocatorCore.c \
  deps/sljit/sljit_src/allocator_src/sljitExecAllocatorFreeBSD.c \
  deps/sljit/sljit_src/allocator_src/sljitExecAllocatorPosix.c \
  deps/sljit/sljit_src/allocator_src/sljitExecAllocatorWindows.c \
  deps/sljit/sljit_src/allocator_src/sljitProtExecAllocatorNetBSD.c \
  deps/sljit/sljit_src/allocator_src/sljitProtExecAllocatorPosix.c \
  deps/sljit/sljit_src/allocator_src/sljitWXExecAllocatorPosix.c \
  deps/sljit/sljit_src/allocator_src/sljitWXExecAllocatorWindows.c

if WITH_PCRE2_8
libpcre2_8_la_LDFLAGS = $(EXTRA_LIBPCRE2_8_LDFLAGS)
endif # WITH_PCRE2_8
if WITH_PCRE2_16
libpcre2_16_la_LDFLAGS = $(EXTRA_LIBPCRE2_16_LDFLAGS)
endif # WITH_PCRE2_16
if WITH_PCRE2_32
libpcre2_32_la_LDFLAGS = $(EXTRA_LIBPCRE2_32_LDFLAGS)
endif # WITH_PCRE2_32

if WITH_VALGRIND
if WITH_PCRE2_8
libpcre2_8_la_CFLAGS += $(VALGRIND_CFLAGS)
endif # WITH_PCRE2_8
if WITH_PCRE2_16
libpcre2_16_la_CFLAGS += $(VALGRIND_CFLAGS)
endif # WITH_PCRE2_16
if WITH_PCRE2_32
libpcre2_32_la_CFLAGS += $(VALGRIND_CFLAGS)
endif # WITH_PCRE2_32
endif # WITH_VALGRIND

if WITH_GCOV
if WITH_PCRE2_8
libpcre2_8_la_CFLAGS += $(GCOV_CFLAGS)
endif # WITH_PCRE2_8
if WITH_PCRE2_16
libpcre2_16_la_CFLAGS += $(GCOV_CFLAGS)
endif # WITH_PCRE2_16
if WITH_PCRE2_32
libpcre2_32_la_CFLAGS += $(GCOV_CFLAGS)
endif # WITH_PCRE2_32
endif # WITH_GCOV

## A version of the 8-bit library that has a POSIX API.

if WITH_PCRE2_8
lib_LTLIBRARIES += libpcre2-posix.la
libpcre2_posix_la_SOURCES = src/pcre2posix.c
libpcre2_posix_la_CFLAGS = \
  -DPCRE2_CODE_UNIT_WIDTH=8 @PCRE2POSIX_CFLAG@ \
  $(VISIBILITY_CFLAGS) $(AM_CFLAGS)
libpcre2_posix_la_LDFLAGS = $(EXTRA_LIBPCRE2_POSIX_LDFLAGS)
libpcre2_posix_la_LIBADD = libpcre2-8.la
if WITH_GCOV
libpcre2_posix_la_CFLAGS += $(GCOV_CFLAGS)
endif # WITH_GCOV
endif # WITH_PCRE2_8

## Build pcre2grep and optional fuzzer stuff if the 8-bit library is enabled

if WITH_PCRE2_8
bin_PROGRAMS += pcre2grep
pcre2grep_SOURCES = src/pcre2grep.c
pcre2grep_CFLAGS = $(AM_CFLAGS)
pcre2grep_LDADD = $(LIBZ) $(LIBBZ2)
pcre2grep_LDADD += libpcre2-8.la
if WITH_GCOV
pcre2grep_CFLAGS += $(GCOV_CFLAGS)
pcre2grep_LDADD += $(GCOV_LIBS)
endif # WITH_GCOV
endif # WITH_PCRE2_8

## If fuzzer support is enabled, build a non-distributed library containing the
## fuzzing function. Also build the standalone checking binary from the same
## source but using -DSTANDALONE.

if WITH_FUZZ_SUPPORT
noinst_LIBRARIES =
if WITH_PCRE2_8
noinst_LIBRARIES += .libs/libpcre2-fuzzsupport.a
_libs_libpcre2_fuzzsupport_a_SOURCES = src/pcre2_fuzzsupport.c
_libs_libpcre2_fuzzsupport_a_CFLAGS = $(AM_CFLAGS)
_libs_libpcre2_fuzzsupport_a_LIBADD =

noinst_PROGRAMS += pcre2fuzzcheck-8
pcre2fuzzcheck_8_SOURCES = src/pcre2_fuzzsupport.c
pcre2fuzzcheck_8_CFLAGS = -DSTANDALONE $(AM_CFLAGS)
pcre2fuzzcheck_8_LDADD = libpcre2-8.la
if WITH_GCOV
pcre2fuzzcheck_8_CFLAGS += $(GCOV_CFLAGS)
pcre2fuzzcheck_8_LDADD += $(GCOV_LIBS)
endif # WITH_GCOV
endif # WITH_PCRE2_8

if WITH_PCRE2_16
noinst_LIBRARIES += .libs/libpcre2-fuzzsupport-16.a
_libs_libpcre2_fuzzsupport_16_a_SOURCES = src/pcre2_fuzzsupport.c
_libs_libpcre2_fuzzsupport_16_a_CFLAGS = $(AM_CFLAGS) -DPCRE2_CODE_UNIT_WIDTH=16
_libs_libpcre2_fuzzsupport_16_a_LIBADD =

noinst_PROGRAMS += pcre2fuzzcheck-16
pcre2fuzzcheck_16_SOURCES = src/pcre2_fuzzsupport.c
pcre2fuzzcheck_16_CFLAGS = -DSTANDALONE $(AM_CFLAGS) -DPCRE2_CODE_UNIT_WIDTH=16
pcre2fuzzcheck_16_LDADD = libpcre2-16.la
if WITH_GCOV
pcre2fuzzcheck_16_CFLAGS += $(GCOV_CFLAGS)
pcre2fuzzcheck_16_LDADD += $(GCOV_LIBS)
endif # WITH_GCOV
endif # WITH_PCRE2_16

if WITH_PCRE2_32
noinst_LIBRARIES += .libs/libpcre2-fuzzsupport-32.a
_libs_libpcre2_fuzzsupport_32_a_SOURCES = src/pcre2_fuzzsupport.c
_libs_libpcre2_fuzzsupport_32_a_CFLAGS = $(AM_CFLAGS) -DPCRE2_CODE_UNIT_WIDTH=32
_libs_libpcre2_fuzzsupport_32_a_LIBADD =

noinst_PROGRAMS += pcre2fuzzcheck-32
pcre2fuzzcheck_32_SOURCES = src/pcre2_fuzzsupport.c
pcre2fuzzcheck_32_CFLAGS = -DSTANDALONE $(AM_CFLAGS) -DPCRE2_CODE_UNIT_WIDTH=32
pcre2fuzzcheck_32_LDADD = libpcre2-32.la
if WITH_GCOV
pcre2fuzzcheck_32_CFLAGS += $(GCOV_CFLAGS)
pcre2fuzzcheck_32_LDADD += $(GCOV_LIBS)
endif # WITH_GCOV
endif # WITH_PCRE2_32

endif # WITH_FUZZ_SUPPORT

## -------- Testing ----------

## If the 8-bit library is enabled, build the POSIX wrapper test program and
## arrange for it to run.

if WITH_PCRE2_8
TESTS += pcre2posix_test
noinst_PROGRAMS += pcre2posix_test
pcre2posix_test_SOURCES = src/pcre2posix_test.c
pcre2posix_test_CFLAGS = $(AM_CFLAGS) @PCRE2POSIX_CFLAG@
pcre2posix_test_LDADD = libpcre2-posix.la libpcre2-8.la
endif # WITH_PCRE2_8

## If JIT support is enabled, arrange for the JIT test program to run.

if WITH_JIT
TESTS += pcre2_jit_test
noinst_PROGRAMS += pcre2_jit_test
pcre2_jit_test_SOURCES = src/pcre2_jit_test.c
pcre2_jit_test_CFLAGS = $(AM_CFLAGS)
pcre2_jit_test_LDADD =
if WITH_PCRE2_8
pcre2_jit_test_LDADD += libpcre2-8.la
endif # WITH_PCRE2_8
if WITH_PCRE2_16
pcre2_jit_test_LDADD += libpcre2-16.la
endif # WITH_PCRE2_16
if WITH_PCRE2_32
pcre2_jit_test_LDADD += libpcre2-32.la
endif # WITH_PCRE2_32
if WITH_GCOV
pcre2_jit_test_CFLAGS += $(GCOV_CFLAGS)
pcre2_jit_test_LDADD += $(GCOV_LIBS)
endif # WITH_GCOV
endif # WITH_JIT

# Build the general pcre2test program.

bin_PROGRAMS += pcre2test
pcre2test_SOURCES = src/pcre2test.c src/pcre2test_inc.h
pcre2test_CFLAGS = $(AM_CFLAGS)
pcre2test_LDADD = $(LIBREADLINE)

if WITH_PCRE2_8
pcre2test_LDADD += libpcre2-8.la libpcre2-posix.la
endif # WITH_PCRE2_8

if WITH_PCRE2_16
pcre2test_LDADD += libpcre2-16.la
endif # WITH_PCRE2_16

if WITH_PCRE2_32
pcre2test_LDADD += libpcre2-32.la
endif # WITH_PCRE2_32

if WITH_VALGRIND
pcre2test_CFLAGS += $(VALGRIND_CFLAGS)
endif # WITH_VALGRIND

if WITH_GCOV
pcre2test_CFLAGS += $(GCOV_CFLAGS)
pcre2test_LDADD += $(GCOV_LIBS)
endif # WITH_GCOV

## The main library tests. Each test is a binary plus a script that runs that
## binary in various ways. We install these test binaries in case folks find it
## helpful. The two .bat files are for running the tests under Windows.

TESTS += RunTest
EXTRA_DIST += RunTest.bat
dist_noinst_SCRIPTS += RunTest

## When the 8-bit library is configured, pcre2grep will have been built.

if WITH_PCRE2_8
TESTS += RunGrepTest
EXTRA_DIST += RunGrepTest.bat
dist_noinst_SCRIPTS += RunGrepTest
if WITH_EBCDIC
XFAIL_TESTS += RunGrepTest
endif # WITH_EBCDIC
endif # WITH_PCRE2_8

## Distribute all the test data files

EXTRA_DIST += \
  testdata/grepbinary \
  testdata/grepfilelist \
  testdata/grepinput \
  testdata/grepinput3 \
  testdata/grepinput8 \
  testdata/grepinputBad8 \
  testdata/grepinputBad8_Trail \
  testdata/grepinputC.bz2 \
  testdata/grepinputC.gz \
  testdata/grepinputM \
  testdata/grepinputUN \
  testdata/grepinputv \
  testdata/grepinputx \
  testdata/greplist \
  testdata/grepnot.bz2 \
  testdata/grepoutput \
  testdata/grepoutput8 \
  testdata/grepoutputC \
  testdata/grepoutputCN \
  testdata/grepoutputCNU \
  testdata/grepoutputCU \
  testdata/grepoutputCbz2 \
  testdata/grepoutputCgz \
  testdata/grepoutputN \
  testdata/grepoutputUN \
  testdata/greppatN4 \
  testdata/testbtables \
  testdata/testinput1 \
  testdata/testinput2 \
  testdata/testinput3 \
  testdata/testinput4 \
  testdata/testinput5 \
  testdata/testinput6 \
  testdata/testinput7 \
  testdata/testinput8 \
  testdata/testinput9 \
  testdata/testinput10 \
  testdata/testinput11 \
  testdata/testinput12 \
  testdata/testinput13 \
  testdata/testinput14 \
  testdata/testinput15 \
  testdata/testinput16 \
  testdata/testinput17 \
  testdata/testinput18 \
  testdata/testinput19 \
  testdata/testinput20 \
  testdata/testinput21 \
  testdata/testinput22 \
  testdata/testinput23 \
  testdata/testinput24 \
  testdata/testinput25 \
  testdata/testinput26 \
  testdata/testinput27 \
  testdata/testinput28 \
  testdata/testinput29 \
  testdata/testinputheap \
  testdata/testoutput1 \
  testdata/testoutput2 \
  testdata/testoutput3 \
  testdata/testoutput3A \
  testdata/testoutput3B \
  testdata/testoutput4 \
  testdata/testoutput5 \
  testdata/testoutput6 \
  testdata/testoutput7 \
  testdata/testoutput8-16-2 \
  testdata/testoutput8-16-4 \
  testdata/testoutput8-32-4 \
  testdata/testoutput8-8-2 \
  testdata/testoutput8-8-3 \
  testdata/testoutput8-8-4 \
  testdata/testoutput9 \
  testdata/testoutput10 \
  testdata/testoutput11-16 \
  testdata/testoutput11-32 \
  testdata/testoutput12-16 \
  testdata/testoutput12-32 \
  testdata/testoutput13 \
  testdata/testoutput14-16 \
  testdata/testoutput14-32 \
  testdata/testoutput14-8 \
  testdata/testoutput15 \
  testdata/testoutput16 \
  testdata/testoutput17 \
  testdata/testoutput18 \
  testdata/testoutput19 \
  testdata/testoutput20 \
  testdata/testoutput21 \
  testdata/testoutput22-16 \
  testdata/testoutput22-32 \
  testdata/testoutput22-8 \
  testdata/testoutput23 \
  testdata/testoutput24 \
  testdata/testoutput25 \
  testdata/testoutput26 \
  testdata/testoutput27 \
  testdata/testoutput28 \
  testdata/testoutput29 \
  testdata/testoutputheap-16 \
  testdata/testoutputheap-32 \
  testdata/testoutputheap-8 \
  testdata/valgrind-jit.supp \
  testdata/wintestinput3 \
  testdata/wintestoutput3 \
  perltest.sh

# RunTest and RunGrepTest should clean up after themselves, but just in case
# they don't, add their working files to CLEANFILES.

CLEANFILES += \
  testSinput \
  testSoutput \
  test3input \
  test3output \
  test3outputA \
  test3outputB \
  testtry \
  teststdout \
  teststderr \
  teststderrgrep \
  testtemp1grep \
  testtemp2grep \
  testtrygrep \
  testNinputgrep

testoutput-clean:
	-rm -rf testoutput8 testoutput8-jit testoutput8-dfa
	-rm -rf testoutput16 testoutput16-jit testoutput16-dfa
	-rm -rf testoutput32 testoutput32-jit testoutput32-dfa

CLEAN_LOCAL_TARGETS += testoutput-clean
PHONY_TARGETS += testoutput-clean

## ------------ End of testing -------------


# PCRE2 demonstration program. Not built automatically. The point is that the
# users should build it themselves. So just distribute the source.

EXTRA_DIST += src/pcre2demo.c


# We have .pc files for pkg-config users.

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA =

if WITH_PCRE2_8
pkgconfig_DATA += libpcre2-8.pc libpcre2-posix.pc
endif

if WITH_PCRE2_16
pkgconfig_DATA += libpcre2-16.pc
endif

if WITH_PCRE2_32
pkgconfig_DATA += libpcre2-32.pc
endif


# Symbol version files for use with GNU and Sun ld.

EXTRA_DIST += \
  src/libpcre2-8.sym src/libpcre2-16.sym src/libpcre2-32.sym \
  src/libpcre2-posix.sym


# gcov/lcov code coverage reporting
#
# Coverage reporting targets:
#
# coverage: Create a coverage report from 'make check'
# coverage-baseline: Capture baseline coverage information
# coverage-reset: This zeros the coverage counters only
# coverage-report: This creates the coverage report only
# coverage-clean-report: This removes the generated coverage report
#   without cleaning the coverage data itself
# coverage-clean-data: This removes the captured coverage data without
#   removing the coverage files created at compile time (*.gcno)
# coverage-clean: This cleans all coverage data including the generated
#   coverage report.

if WITH_GCOV
COVERAGE_TEST_NAME = $(PACKAGE)
COVERAGE_NAME = $(PACKAGE)-$(VERSION)
COVERAGE_OUTPUT_FILE = $(COVERAGE_NAME)-coverage.info
COVERAGE_OUTPUT_DIR = $(COVERAGE_NAME)-coverage
COVERAGE_LCOV_EXTRA_FLAGS =
COVERAGE_GENHTML_EXTRA_FLAGS =

coverage_quiet = $(coverage_quiet_$(V))
coverage_quiet_ = $(coverage_quiet_$(AM_DEFAULT_VERBOSITY))
coverage_quiet_0 = --quiet

coverage-check: all
	-$(MAKE) $(AM_MAKEFLAGS) -k check

coverage-baseline:
	$(LCOV) $(coverage_quiet) \
		--directory $(top_builddir) \
		--output-file "$(COVERAGE_OUTPUT_FILE)" \
		--capture \
		--initial

coverage-report:
	$(LCOV) $(coverage_quiet) \
		--directory $(top_builddir) \
		--capture \
		--output-file "$(COVERAGE_OUTPUT_FILE).tmp" \
		--test-name "$(COVERAGE_TEST_NAME)" \
		--no-checksum \
		--compat-libtool \
		$(COVERAGE_LCOV_EXTRA_FLAGS)
	$(LCOV) $(coverage_quiet) \
		--directory $(top_builddir) \
		--output-file "$(COVERAGE_OUTPUT_FILE)" \
		--remove "$(COVERAGE_OUTPUT_FILE).tmp" \
		"/tmp/*" \
		"/usr/include/*" \
		"$(includedir)/*"
	-@rm -f "$(COVERAGE_OUTPUT_FILE).tmp"
	LANG=C $(GENHTML) $(coverage_quiet) \
		--prefix $(top_builddir) \
		--output-directory "$(COVERAGE_OUTPUT_DIR)" \
		--title "$(PACKAGE) $(VERSION) Code Coverage Report" \
		--show-details "$(COVERAGE_OUTPUT_FILE)" \
		--legend \
		$(COVERAGE_GENHTML_EXTRA_FLAGS)
	@echo "Code coverage report written to file://$(abs_builddir)/$(COVERAGE_OUTPUT_DIR)/index.html"

coverage-reset:
	-$(LCOV) $(coverage_quiet) --zerocounters --directory $(top_builddir)

coverage-clean-report:
	-rm -f "$(COVERAGE_OUTPUT_FILE)" "$(COVERAGE_OUTPUT_FILE).tmp"
	-rm -rf "$(COVERAGE_OUTPUT_DIR)"

coverage-clean-data:
	-find $(top_builddir) -name "*.gcda" -delete

coverage-clean: coverage-reset coverage-clean-report coverage-clean-data
	-find $(top_builddir) -name "*.gcno" -delete

coverage-distclean: coverage-clean

coverage: coverage-reset coverage-baseline coverage-check coverage-report

CLEAN_LOCAL_TARGETS += coverage-clean
DISTCLEAN_LOCAL_TARGETS += coverage-distclean
PHONY_TARGETS += coverage coverage-baseline coverage-check coverage-report coverage-reset coverage-clean-report coverage-clean-data coverage-clean coverage-distclean

else

coverage:
	@echo "Configuring with --enable-coverage is required to generate code coverage report."

# Without coverage support, still arrange for 'make distclean' to get rid of
# any coverage files that may have been left from a different configuration.

DISTCLEANFILES += src/*.gcda src/*.gcno

coverage-distclean:
	rm -rf $(PACKAGE)-$(VERSION)-coverage*

DISTCLEAN_LOCAL_TARGETS += coverage-distclean
PHONY_TARGETS += coverage-distclean

endif # WITH_GCOV

## CMake support

EXTRA_DIST += \
  cmake/COPYING-CMAKE-SCRIPTS \
  cmake/FindEditline.cmake \
  cmake/FindReadline.cmake \
  cmake/pcre2-config.cmake.in \
  cmake/PCRE2CheckVscript.cmake \
  cmake/PCRE2UseSystemExtensions.cmake \
  cmake/PCRE2WarningAsError.cmake \
  src/config-cmake.h.in \
  CMakeLists.txt

## Set the special make and Automake targets. We could very easily simply
## redefine these targets to append prerequisites to them, rather than
## collecting the prerequisites in variables. However, there is an annoying
## Automake behaviour where it emits a warning if a target has prerequisites
## appended in a conditional block.

clean-local: $(CLEAN_LOCAL_TARGETS)
distclean-local: $(DISTCLEAN_LOCAL_TARGETS)
.PHONY: $(PHONY_TARGETS)

## end Makefile.am
