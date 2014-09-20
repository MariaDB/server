htmldir = $(pkgdatadir)/html

include files.am

EXTRA_DIST =					\
	CMakeLists.txt

update-files:
	cd $(srcdir); ./update-files.sh > files.am
