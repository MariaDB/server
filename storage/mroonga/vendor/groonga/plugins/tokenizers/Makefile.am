EXTRA_DIST =					\
	CMakeLists.txt

AM_CPPFLAGS =			\
	-I$(top_builddir)	\
	-I$(top_srcdir)/include	\
	-I$(top_srcdir)/lib

AM_LDFLAGS =					\
	-avoid-version				\
	-module					\
	-no-undefined

LIBS =						\
	$(top_builddir)/lib/libgroonga.la

tokenizer_plugins_LTLIBRARIES =
if WITH_MECAB
tokenizer_plugins_LTLIBRARIES += mecab.la
endif
if WITH_KYTEA
tokenizer_plugins_LTLIBRARIES += kytea.la
endif

include mecab_sources.am
mecab_la_CPPFLAGS = $(AM_CPPFLAGS) $(MECAB_CPPFLAGS)
mecab_la_LIBADD = $(LIBS) $(MECAB_LIBS)
mecab_la_LDFLAGS = $(AM_LDFLAGS) $(MECAB_LDFLAGS)

include kytea_sources.am
kytea_la_CPPFLAGS = $(AM_CPPFLAGS) $(KYTEA_CFLAGS)
kytea_la_LIBADD = $(LIBS) $(KYTEA_LIBS)
kytea_la_LDFLAGS = $(AM_LDFLAGS) $(KYTEA_LDFLAGS)
