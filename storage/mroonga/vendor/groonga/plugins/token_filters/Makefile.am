EXTRA_DIST =					\
	CMakeLists.txt

AM_CPPFLAGS =					\
	-I$(top_builddir)			\
	-I$(top_srcdir)/include			\
	-I$(top_srcdir)/lib

AM_LDFLAGS =					\
	-avoid-version				\
	-module					\
	-no-undefined

LIBS =						\
	$(top_builddir)/lib/libgroonga.la

token_filter_plugins_LTLIBRARIES =
token_filter_plugins_LTLIBRARIES += stop_word.la
if WITH_LIBSTEMMER
token_filter_plugins_LTLIBRARIES += stem.la
endif

include stop_word_sources.am

include stem_sources.am
stem_la_CPPFLAGS = $(AM_CPPFLAGS) $(LIBSTEMMER_CFLAGS)
stem_la_LIBADD = $(LIBS) $(LIBSTEMMER_LIBS)
stem_la_LDFLAGS = $(AM_LDFLAGS) $(LIBSTEMMER_LDFLAGS)
