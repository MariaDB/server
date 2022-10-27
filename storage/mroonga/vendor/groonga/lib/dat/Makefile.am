DEFS += -D_REENTRANT $(GRN_DEFS) -DGRN_DAT_EXPORT

DEFAULT_INCLUDES =				\
	-I$(top_builddir)			\
	-I$(top_srcdir)/include

noinst_LTLIBRARIES = libgrndat.la

include sources.am

CLEANFILES = *.gcno *.gcda
