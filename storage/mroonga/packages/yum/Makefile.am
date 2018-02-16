REPOSITORIES_PATH = repositories
DISTRIBUTIONS = centos
ARCHITECTURES = i386 x86_64
MYSQL_VARIANTS =				\
	mysql55					\
	mysql56-community			\
	mysql57-community			\
	mariadb					\
	mariadb-10.1				\
	mariadb-10.2				\
	percona-server-56			\
	percona-server-57
CENTOS_VERSIONS = 6 7
SPEC_DIR = $(builddir)/../rpm/centos

all:

release: download build sign-packages update-repository upload

remove-existing-packages:
	for distribution in $(DISTRIBUTIONS); do	\
	  find $${distribution} -name "*.rpm" -delete;	\
	done

ensure-rsync-path:
	@if test -z "$(RSYNC_PATH)"; then				\
	  echo "--with-rsync-path configure option must be specified.";	\
	  false;							\
	fi

sign-packages:
	./sign-rpm.sh '$(GPG_UID)' '$(REPOSITORIES_PATH)/' '$(DISTRIBUTIONS)'

update-repository:
	./update-repository.sh '$(REPOSITORIES_PATH)/' '$(DISTRIBUTIONS)'

upload: ensure-rsync-path
	for distribution in $(DISTRIBUTIONS); do		\
	  rsync -avz --progress --delete --exclude .gitignore	\
	    $(REPOSITORIES_PATH)/$${distribution}/		\
	    $(RSYNC_PATH)/$${distribution};			\
	done

download: ensure-rsync-path
	mkdir -p $(REPOSITORIES_PATH)
	for distribution in $(DISTRIBUTIONS); do	\
	  rsync -avz --progress --delete		\
	    $(RSYNC_PATH)/$${distribution}/		\
	    $(REPOSITORIES_PATH)/$${distribution};	\
	done

build: build-in-vm

build-in-vm: source specs env.sh
	./build-in-vm.sh			\
	  "$(PACKAGE)"				\
	  "$(SPEC_DIR)"				\
	  "$(MYSQL_VARIANTS)"			\
	  "$(ARCHITECTURES)"			\
	  "$(CENTOS_VERSIONS)"

source: tmp/$(PACKAGE)-$(VERSION).tar.gz

tmp/$(PACKAGE)-$(VERSION).tar.gz: $(abs_top_builddir)/$(PACKAGE)-$(VERSION).tar.gz
	mkdir -p tmp/
	cp $(abs_top_builddir)/$(PACKAGE)-$(VERSION).tar.gz tmp/

$(abs_top_builddir)/$(PACKAGE)-$(VERSION).tar.gz:
	cd $(abs_top_builddir) && $(MAKE) dist

specs: $(SPEC_DIR)/mysql55-$(PACKAGE).spec
specs: $(SPEC_DIR)/mysql56-community-$(PACKAGE).spec
specs: $(SPEC_DIR)/mariadb-$(PACKAGE).spec
specs: $(SPEC_DIR)/mariadb-10.1-$(PACKAGE).spec
specs: $(SPEC_DIR)/mariadb-10.2-$(PACKAGE).spec
specs: $(SPEC_DIR)/percona-server-56-$(PACKAGE).spec
specs: $(SPEC_DIR)/percona-server-57-$(PACKAGE).spec
