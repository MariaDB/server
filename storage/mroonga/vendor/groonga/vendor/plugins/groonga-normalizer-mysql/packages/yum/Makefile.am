REPOSITORIES_PATH = repositories
DISTRIBUTIONS = centos
ARCHITECTURES = i386 x86_64
CENTOS_VERSIONS = 5 6 7

release: download build sign-packages update-repository upload

ensure-rsync-path:
	@if test -z "$(RSYNC_PATH)"; then				\
	  echo "--with-rsync-path configure option must be specified.";	\
	  false;							\
	fi

sign-packages:
	./sign-rpm.sh '$(GPG_UID)' '$(REPOSITORIES_PATH)/' \
	  '$(DISTRIBUTIONS)'

update-repository: RPM-GPG-KEY-$(PACKAGE)
	./update-repository.sh '$(PACKAGE)' '$(REPOSITORIES_PATH)/' \
	  '$(DISTRIBUTIONS)'

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
	vagrant destroy --force
	for architecture in $(ARCHITECTURES); do		\
	  for version in $(CENTOS_VERSIONS); do			\
	    if [ $$version = 7 -a $$architecture = i386 ]; then	\
	      continue;						\
	    fi;							\
	    id=centos-$$version-$$architecture;			\
	    vagrant up $$id;					\
	    vagrant destroy --force $$id;			\
	  done;							\
	done

ensure-public-key:
	gpg --list-keys '$(GPG_UID)' > /dev/null ||			\
	  gpg --keyserver keyserver.ubuntu.com --recv-key '$(GPG_UID)'

RPM-GPG-KEY-$(PACKAGE): ensure-public-key
	gpg --armor --export '$(GPG_UID)' > $@

source: tmp/$(PACKAGE)-$(VERSION).tar.gz

tmp/$(PACKAGE)-$(VERSION).tar.gz: $(abs_top_builddir)/$(PACKAGE)-$(VERSION).tar.gz
	mkdir -p tmp/
	cp $(abs_top_builddir)/$(PACKAGE)-$(VERSION).tar.gz tmp/

$(abs_top_builddir)/$(PACKAGE)-$(VERSION).tar.gz:
	cd $(abs_top_builddir) && $(MAKE) dist

specs: tmp/centos/$(PACKAGE).spec

tmp/centos/$(PACKAGE).spec: $(builddir)/../rpm/centos/$(PACKAGE).spec
	mkdir -p tmp/centos
	cp $(builddir)/../rpm/centos/$(PACKAGE).spec tmp/centos/
