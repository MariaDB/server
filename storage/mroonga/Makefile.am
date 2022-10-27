ACLOCAL_AMFLAGS = -I m4 ${ACLOCAL_ARGS}
AUTOMAKE_OPTIONS = 1.9.7

LOCALES = ja

AM_CPPFLAGS = $(MYSQL_INCLUDES) $(GROONGA_CFLAGS) -I$(top_srcdir)/lib

include sources.am

libraries =					\
	$(top_builddir)/udf/libmrn_udf.la	\
	$(top_builddir)/lib/libmrn_no_mysql.la	\
	$(top_builddir)/lib/libmrn_need_mysql.la
if WITH_LIBMYSQLSERVICES_COMPAT
libraries += $(top_builddir)/lib/libmysqlservices.la
endif

dynamic_plugin_ldflags  = -module $(GROONGA_LIBS) $(MYSQL_LIBS)
dynamic_plugin_cxxflags = $(AM_CXXFLAGS) $(MYSQL_CFLAGS) -DMYSQL_DYNAMIC_PLUGIN
dynamic_plugin_cflags   = $(AM_CFLAGS) $(MYSQL_CFLAGS) -DMYSQL_DYNAMIC_PLUGIN

plugin_LTLIBRARIES     = ha_mroonga.la
ha_mroonga_la_LDFLAGS  = $(dynamic_plugin_ldflags)
ha_mroonga_la_CXXFLAGS = $(dynamic_plugin_cxxflags)
ha_mroonga_la_CFLAGS   = $(dynamic_plugin_cflags)
ha_mroonga_la_SOURCES  = $(sources)
ha_mroonga_la_LIBADD   = $(libraries)

SUBDIRS =					\
	build					\
	lib					\
	udf					\
	test					\
	mysql-test				\
	doc					\
	tools					\
	packages				\
	data

EXTRA_DIST =					\
	AUTHORS					\
	gpg_uid					\
	CMakeLists.txt

installcheck-local: install
	test/run-sql-test.sh

tag:
	cd $(top_srcdir) && \
	  git tag v$(VERSION) -a -m 'Mroonga $(VERSION)!!!'

ensure-cutter-source-path:
	@if test -z "$(CUTTER_SOURCE_PATH)"; then \
	  echo "\$$(CUTTER_SOURCE_PATH) is missing"; \
	  exit 1; \
	fi

update-latest-release: ensure-cutter-source-path
	@if test -z "$(OLD_RELEASE)"; then	\
	  echo "\$$(OLD_RELEASE) is missing";	\
	  exit 1;				\
	fi
	@if test -z "$(OLD_RELEASE_DATE)"; then		\
	  echo "\$$(OLD_RELEASE_DATE) is missing";	\
	  exit 1;					\
	fi
	@if test -z "$(NEW_RELEASE_DATE)"; then		\
	  echo "\$$(NEW_RELEASE_DATE) is missing";	\
	  exit 1;					\
	fi
	cd $(top_srcdir) &&				\
	  "$(CUTTER_SOURCE_PATH)/misc/update-latest-release.rb"	\
	  $(PACKAGE) $(OLD_RELEASE) $(OLD_RELEASE_DATE)	\
	  $(VERSION) $(NEW_RELEASE_DATE)		\
	  packages/rpm/centos/mariadb-mroonga.spec.in	\
	  packages/rpm/centos/mariadb-10.1-mroonga.spec.in	\
	  packages/rpm/centos/mariadb-10.2-mroonga.spec.in	\
	  packages/rpm/centos/mysql55-mroonga.spec.in	\
	  packages/rpm/centos/mysql56-community-mroonga.spec.in	\
	  packages/rpm/centos/mysql57-community-mroonga.spec.in	\
	  packages/rpm/centos/percona-server-56-mroonga.spec.in	\
	  packages/rpm/centos/percona-server-57-mroonga.spec.in	\
	  doc/source/install/*.rst			\
	  doc/locale/*/LC_MESSAGES/install.po		\
	  $(MROONGA_GITHUB_COM_PATH)/_config.yml
	cd $(top_srcdir) &&				\
	  "$(CUTTER_SOURCE_PATH)/misc/update-latest-release.rb"	\
	  $(PACKAGE)-5.5 $(OLD_RELEASE) $(OLD_RELEASE_DATE)	\
	  $(VERSION) $(NEW_RELEASE_DATE)		\
	  packages/debian-5.5/changelog
	cd $(top_srcdir) &&				\
	  "$(CUTTER_SOURCE_PATH)/misc/update-latest-release.rb"	\
	  $(PACKAGE)-5.6 $(OLD_RELEASE) $(OLD_RELEASE_DATE)	\
	  $(VERSION) $(NEW_RELEASE_DATE)		\
	  packages/debian-5.6/changelog
	cd $(top_srcdir) &&				\
	  "$(CUTTER_SOURCE_PATH)/misc/update-latest-release.rb"	\
	  $(PACKAGE)-5.7 $(OLD_RELEASE) $(OLD_RELEASE_DATE)	\
	  $(VERSION) $(NEW_RELEASE_DATE)		\
	  packages/debian-5.7/changelog
	cd $(top_srcdir) &&				\
	  "$(CUTTER_SOURCE_PATH)/misc/update-latest-release.rb"	\
	  $(PACKAGE)-mariadb-10.0 $(OLD_RELEASE) $(OLD_RELEASE_DATE)	\
	  $(VERSION) $(NEW_RELEASE_DATE)		\
	  packages/debian-mariadb-10.0/changelog

update-po:
	@for lang in $(LOCALES); do						\
	  (cd $(top_srcdir)/doc/locale/$$lang/LC_MESSAGES && make update)	\
	done

update-document:
	@if test -z "$(MROONGA_GITHUB_COM_PATH)"; then			\
	  echo "\$$(MROONGA_GITHUB_COM_PATH) is missing";		\
	  echo "add --with-mroonga-github-com-path in configure";	\
	  exit 1;							\
	fi
	rm -rf tmp-doc
	mkdir tmp-doc
	(cd doc && $(MAKE) clean-html)
	(cd doc && $(MAKE) install docdir=$(abs_srcdir)/tmp-doc/install)
	ruby $(srcdir)/tools/prepare-sphinx-html.rb tmp-doc/install tmp-doc/dist
	rm -rf $(MROONGA_GITHUB_COM_PATH)/docs
	mv tmp-doc/dist/en $(MROONGA_GITHUB_COM_PATH)/docs
	for locale in `cd tmp-doc/dist; echo *`; do		\
	  dest_base_dir=$(MROONGA_GITHUB_COM_PATH)/$${locale};	\
	  mkdir -p $${dest_base_dir};				\
	  dest_dir=$${dest_base_dir}/docs;			\
	  rm -rf $${dest_dir};					\
	  mv tmp-doc/dist/$${locale} $${dest_dir};		\
	done

update-files:
	cd $(srcdir)/doc && $(MAKE) update-files

update-version:
	@if test -z "$(NEW_VERSION_MAJOR)"; then	\
	  echo "\$$(NEW_VERSION_MAJOR) is missing";	\
	  exit 1;					\
	fi
	@if test -z "$(NEW_VERSION_MINOR)"; then	\
	  echo "\$$(NEW_VERSION_MINOR) is missing";	\
	  exit 1;					\
	fi
	@if test -z "$(NEW_VERSION_MICRO)"; then	\
	  echo "\$$(NEW_VERSION_MICRO) is missing";	\
	  exit 1;					\
	fi
	@echo -n $(NEW_VERSION_MAJOR) > $(srcdir)/version_major
	@echo -n $(NEW_VERSION_MINOR) > $(srcdir)/version_minor
	@echo -n $(NEW_VERSION_MICRO) > $(srcdir)/version_micro
	@echo -n $(NEW_VERSION_MAJOR).$(NEW_VERSION_MINOR)$(NEW_VERSION_MICRO) \
	  > $(srcdir)/version
	@if test $(NEW_VERSION_MINOR) -eq 0 ; then				\
	  printf "0x%02x%02x"							\
	    $(NEW_VERSION_MAJOR) $(NEW_VERSION_MICRO)				\
	    > $(srcdir)/version_in_hex;						\
	  printf "%d.%d"							\
	    $(NEW_VERSION_MAJOR) $(NEW_VERSION_MICRO)				\
	    > $(srcdir)/plugin_version;						\
	else									\
	  printf "0x%02x%02x"							\
	    $(NEW_VERSION_MAJOR) $(NEW_VERSION_MINOR)$(NEW_VERSION_MICRO)	\
	    > $(srcdir)/version_in_hex;						\
	  printf "%d.%d"							\
	    $(NEW_VERSION_MAJOR) $(NEW_VERSION_MINOR)$(NEW_VERSION_MICRO)	\
	    > $(srcdir)/plugin_version;						\
	fi

upload-to-github:
	ruby $(srcdir)/tools/upload-to-github.rb \
	  $$USER $(PACKAGE)-$(VERSION).tar.gz

echo-cutter:
	echo $(CUTTER)
