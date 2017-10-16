MROONGA_BASE = $(PACKAGE)-$(VERSION)
MROONGA_TAR_GZ = $(MROONGA_BASE).tar.gz

GROONGA_VERSION = 7.0.7
GROONGA_BASE = groonga-$(GROONGA_VERSION)
GROONGA_TAR_GZ = $(GROONGA_BASE).tar.gz

GROONGA_NORMALIZER_MYSQL_VERSION = 1.1.1
GROONGA_NORMALIZER_MYSQL_BASE = \
	groonga-normalizer-mysql-$(GROONGA_NORMALIZER_MYSQL_VERSION)
GROONGA_NORMALIZER_MYSQL_TAR_GZ = \
	$(GROONGA_NORMALIZER_MYSQL_BASE).tar.gz

MARIADB_VERSION = 10.1.28
MARIADB_BASE = mariadb-$(MARIADB_VERSION)
MARIADB_TAR_GZ = $(MARIADB_BASE).tar.gz

MARIADB_WITH_MROONGA_BASE = $(MARIADB_BASE)-with-$(MROONGA_BASE)
MARIADB_WITH_MROONGA_FOR_WINDOWS_BASE = $(MARIADB_WITH_MROONGA_BASE)-for-windows

GROONGA_PROJECT_DOWNLOAD_BASE = http://packages.groonga.org/source
GROONGA_DOWNLOAD_BASE = $(GROONGA_PROJECT_DOWNLOAD_BASE)/groonga
GROONGA_NORMALIZER_MYSQL_DOWNLOAD_BASE = \
	$(GROONGA_PROJECT_DOWNLOAD_BASE)/groonga-normalizer-mysql
MARIADB_DOWNLOAD_BASE = http://ftp.yz.yamagata-u.ac.jp/pub/dbms/mariadb


CURL = curl --fail --silent --show-error

all:

release: download archive upload

ensure-rsync-path:
	@if test -z "$(RSYNC_PATH)"; then				\
	  echo "--with-rsync-path configure option must be specified.";	\
	  false;							\
	fi

download: ensure-rsync-path
	rsync -avz --progress --delete $(RSYNC_PATH)/source/mroonga/ files

ARCHIVES =							\
	files/$(MROONGA_TAR_GZ)					\
	files/$(MARIADB_WITH_MROONGA_BASE).tar.gz		\
	files/$(MARIADB_WITH_MROONGA_FOR_WINDOWS_BASE).zip

archive: $(ARCHIVES)

upload: ensure-rsync-path
	rsync -avz --progress --delete files/ $(RSYNC_PATH)/source/mroonga

files/$(MROONGA_TAR_GZ): $(top_builddir)/$(MROONGA_TAR_GZ)
	mkdir -p files
	cp -p $< $@

tmp/$(GROONGA_TAR_GZ):
	mkdir -p tmp
	$(CURL) --output $@ $(GROONGA_DOWNLOAD_BASE)/$(GROONGA_TAR_GZ)

tmp/$(GROONGA_NORMALIZER_MYSQL_TAR_GZ):
	mkdir -p tmp
	$(CURL) --output $@ $(GROONGA_NORMALIZER_MYSQL_DOWNLOAD_BASE)/$(GROONGA_NORMALIZER_MYSQL_TAR_GZ)

tmp/$(MARIADB_TAR_GZ):
	mkdir -p tmp
	$(CURL) --output $@ $(MARIADB_DOWNLOAD_BASE)/mariadb-$(MARIADB_VERSION)/source/$(MARIADB_TAR_GZ)

MARIADB_WITH_MROONGA_ARCHIVES =			\
	tmp/$(GROONGA_TAR_GZ)			\
	tmp/$(GROONGA_NORMALIZER_MYSQL_TAR_GZ)	\
	tmp/$(MARIADB_TAR_GZ)			\
	$(top_builddir)/$(MROONGA_TAR_GZ)

BUNDLED_MROONGA_PATH = $(MARIADB_BASE)/storage/$(PACKAGE)
BUNDLED_GROONGA_PATH = $(BUNDLED_MROONGA_PATH)/vendor/groonga
BUNDLED_GROONGA_NORMALIZER_MYSQL_PATH = \
	$(BUNDLED_GROONGA_PATH)/vendor/plugins/groonga-normalizer-mysql

tmp/$(MARIADB_WITH_MROONGA_BASE).stamp: $(MARIADB_WITH_MROONGA_ARCHIVES)
	rm -rf $(MARIADB_BASE)
	tar xf tmp/$(MARIADB_TAR_GZ)

	rm -fr $(MARIADB_BASE)/storage/mroonga
	tar xf $(top_builddir)/$(MROONGA_TAR_GZ)
	mv $(MROONGA_BASE) $(BUNDLED_MROONGA_PATH)

	mkdir -p $$(dirname $(BUNDLED_GROONGA_PATH))
	tar xf tmp/$(GROONGA_TAR_GZ)
	rm -rf $(GROONGA_BASE)/test
	cd $(GROONGA_BASE)/vendor && ruby download_mecab.rb
	cd $(GROONGA_BASE)/vendor && ruby download_lz4.rb
	mv $(GROONGA_BASE) $(BUNDLED_GROONGA_PATH)

	tar xf tmp/$(GROONGA_NORMALIZER_MYSQL_TAR_GZ)
	rm -rf $(GROONGA_NORMALIZER_MYSQL_BASE)/test
	mv $(GROONGA_NORMALIZER_MYSQL_BASE) $(BUNDLED_GROONGA_NORMALIZER_MYSQL_PATH)

	rm -rf tmp/$(MARIADB_WITH_MROONGA_BASE)
	mv $(MARIADB_BASE) tmp/$(MARIADB_WITH_MROONGA_BASE)

	touch $@

files/$(MARIADB_WITH_MROONGA_BASE).tar.gz: tmp/$(MARIADB_WITH_MROONGA_BASE).stamp
	mkdir -p files/
	(cd tmp && tar czf ../$@ $(MARIADB_WITH_MROONGA_BASE))

PATCHES =								\
	patches/mariadb-10.0.3-windows-build.diff

tmp/$(MARIADB_WITH_MROONGA_FOR_WINDOWS_BASE).stamp: tmp/$(MARIADB_WITH_MROONGA_BASE).stamp $(PATCHES)
	rm -rf tmp/$(MARIADB_WITH_MROONGA_FOR_WINDOWS_BASE)
	cp -a						\
	  tmp/$(MARIADB_WITH_MROONGA_BASE)		\
	  tmp/$(MARIADB_WITH_MROONGA_FOR_WINDOWS_BASE)
	for patch in $(PATCHES); do				\
	  (cd tmp/$(MARIADB_WITH_MROONGA_FOR_WINDOWS_BASE) &&	\
	    patch -p1 < $(abs_srcdir)/$${patch});		\
	done

	touch $@

files/$(MARIADB_WITH_MROONGA_FOR_WINDOWS_BASE).zip: tmp/$(MARIADB_WITH_MROONGA_FOR_WINDOWS_BASE).stamp
	mkdir -p files/
	(cd tmp && zip -q -r ../$@ $(MARIADB_WITH_MROONGA_FOR_WINDOWS_BASE))
