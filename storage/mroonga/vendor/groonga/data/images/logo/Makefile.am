include files.am

logodir = $(pkgdatadir)/images/logo
logo_DATA = $(image_files)

EXTRA_DIST =					\
	$(logo_DATA)				\
	update-files.sh

SUFFIXES =
if WITH_INKSCAPE
SUFFIXES += .svg .png
.svg.png:
	$(INKSCAPE) --export-dpi 90 --export-png $@ $<
endif

$(srcdir)/files.am: $(srcdir)/update-files.sh
	(cd $(srcdir) && ./update-files.sh) > $(srcdir)/files.am

update-files:
	rm $(srcdir)/files.am
	$(MAKE) $(srcdir)/files.am
