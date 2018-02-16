# release: update-latest-release (commit) tag

LOCALES = ja

ACLOCAL_AMFLAGS = -I m4
AUTOMAKE_OPTIONS = 1.9.6
SUBDIRS =					\
	build					\
	include					\
	vendor					\
	lib					\
	plugins					\
	src					\
	examples				\
	test					\
	benchmark				\
	packages				\
	data					\
	tools					\
	doc
#dist_data_DATA =
EXTRA_DIST =					\
	CMakeLists.txt				\
	README.md				\
	base_version				\
	bindings				\
	bundled_lz4_version			\
	bundled_mecab_naist_jdic_version	\
	bundled_mecab_version			\
	config.h.cmake				\
	gpg_uid					\
	nginx_version				\
	version-gen.sh

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = groonga.pc
if GRN_WITH_ARROW
pkgconfig_DATA += groonga-arrow.pc
endif

.PHONY: FORCE

FORCE:

$(srcdir)/version.sh: FORCE
	@$(SHELL_PATH) $(srcdir)/version-gen.sh

include $(srcdir)/version.sh

dist-hook:
	echo "$(GRN_VERSION)" > $(distdir)/version
	cd $(distdir) && autoreconf --install --force && find . -name autom4te.cache | xargs rm -fr

benchmark:
	cd test/benchmark && $(MAKE) benchmark

tag:
	cd $(top_srcdir) && git tag v$(VERSION) -a -m 'Groonga $(VERSION)!!!'

echo-version:
	@echo $(VERSION)

update-latest-release: misc
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
	cd $(top_srcdir) &&					\
	  misc/update-latest-release.rb				\
	  $(PACKAGE) $(OLD_RELEASE) $(OLD_RELEASE_DATE)		\
	  $(VERSION) $(NEW_RELEASE_DATE)			\
	  packages/rpm/centos/groonga.spec.in			\
	  packages/debian/changelog				\
	  doc/source/install.rst				\
	  doc/source/install/*.rst				\
	  doc/locale/*/LC_MESSAGES/install.po			\
	  $(GROONGA_ORG_PATH)/_config.yml

update-po:
	@for lang in $(LOCALES); do						\
	  (cd $(top_srcdir)/doc/locale/$$lang/LC_MESSAGES && $(MAKE) update)	\
	done

update-document:
	@if test -z "$(GROONGA_ORG_PATH)"; then				\
	  echo "\$$(GROONGA_ORG_PATH) is missing";			\
	  echo "add --with-groonga-org-path in configure";	\
	  exit 1;							\
	fi
	rm -rf tmp-doc
	mkdir tmp-doc
	(cd doc && $(MAKE) clean-html)
	(cd doc && $(MAKE) install docdir=$(abs_srcdir)/tmp-doc/install)
	ruby $(srcdir)/tools/prepare-sphinx-html.rb tmp-doc/install tmp-doc/dist
	rm -rf $(GROONGA_ORG_PATH)/docs
	mv tmp-doc/dist/en $(GROONGA_ORG_PATH)/docs
	for locale in `cd tmp-doc/dist; echo *`; do		\
	  dest_base_dir=$(GROONGA_ORG_PATH)/$${locale};		\
	  mkdir -p $${dest_base_dir};				\
	  dest_dir=$${dest_base_dir}/docs;			\
	  rm -rf $${dest_dir};					\
	  mv tmp-doc/dist/$${locale} $${dest_dir};		\
	done


update-version:
	@if test -z "$(NEW_VERSION)"; then	\
	  echo "\$$(NEW_VERSION) is missing";	\
	  exit 1;				\
	fi
	@echo -n $(NEW_VERSION) > $(srcdir)/base_version

update-files:
	cd doc && $(MAKE) update-files
	cd data/html && $(MAKE) update-files

update-mruby:
	cd $(top_srcdir)/vendor/mruby && make update

update-nginx:
	@if test -z "$(NEW_NGINX_VERSION)"; then	\
	  echo "\$$(NEW_NGINX_VERSION) is missing";	\
	  exit 1;					\
	fi
	cd $(top_srcdir)/vendor && ./update_nginx.sh $(NEW_NGINX_VERSION)

misc:
	@if test -z "$(CUTTER_SOURCE_PATH)"; then \
	  echo "\$$(CUTTER_SOURCE_PATH) is missing"; \
	  exit 1; \
	fi
	ln -s "$(CUTTER_SOURCE_PATH)/misc" misc

