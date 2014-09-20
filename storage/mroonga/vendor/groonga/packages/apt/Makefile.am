REPOSITORIES_PATH = repositories
DISTRIBUTIONS = debian
CHROOT_BASE = /var/lib/chroot
ARCHITECTURES = i386 amd64
CODES = wheezy jessie unstable
KEYRING_PACKAGE = groonga-keyring
KEYRING_VERSION = 2012.05.29
KEYRING_BASE_NAME = $(KEYRING_PACKAGE)-$(KEYRING_VERSION)

all:

release: build sign-packages update-repository sign-repository upload

remove-existing-packages:
	for distribution in $(DISTRIBUTIONS); do		\
	  find $(REPOSITORIES_PATH)/$${distribution}/pool	\
	    -type f -delete;					\
	done

ensure-rsync-path:
	@if test -z "$(RSYNC_PATH)"; then				\
	  echo "--with-rsync-path configure option must be specified.";	\
	  false;							\
	fi

download: ensure-rsync-path
	for distribution in $(DISTRIBUTIONS); do	\
	  rsync -avz --progress --delete		\
	    $(RSYNC_PATH)/$${distribution}		\
	    ${REPOSITORIES_PATH}/;	\
	done

sign-packages:
	./sign-packages.sh '$(GPG_UID)' '$(REPOSITORIES_PATH)/' '$(CODES)'

update-repository:
	./update-repository.sh '$(PACKAGE_NAME)' '$(REPOSITORIES_PATH)/' \
	  '$(ARCHITECTURES)' '$(CODES)'

sign-repository:
	./sign-repository.sh '$(GPG_UID)' '$(REPOSITORIES_PATH)/' '$(CODES)'

upload: ensure-rsync-path
	for distribution in $(DISTRIBUTIONS); do		\
	  (cd $(REPOSITORIES_PATH)/$${distribution};		\
	   rsync -avz --progress --delete			\
	     dists pool $(RSYNC_PATH)/$${distribution});	\
	done

build: build-package-deb build-keyring-deb

build-package-deb: source
	./build-in-chroot.sh						\
	  $(PACKAGE) $(VERSION) $(srcdir)/.. $(REPOSITORIES_PATH)/	\
	  $(CHROOT_BASE) '$(ARCHITECTURES)' '$(CODES)'

build-keyring-deb: keyring-source
	./build-in-chroot.sh					\
	  $(KEYRING_PACKAGE) $(KEYRING_VERSION) $(srcdir)	\
	  $(REPOSITORIES_PATH)/ $(CHROOT_BASE)			\
	  '$(ARCHITECTURES)' '$(CODES)'

source: ../$(PACKAGE)-$(VERSION).tar.gz

../$(PACKAGE)-$(VERSION).tar.gz:
	ln -s $(abs_top_builddir)/$(PACKAGE)-$(VERSION).tar.gz ../

keyring-source: $(KEYRING_BASE_NAME).tar.gz

ensure-public-key:
	gpg --list-keys '$(GPG_UID)' > /dev/null ||			\
	  gpg --keyserver keyserver.ubuntu.com --recv-key '$(GPG_UID)'

$(KEYRING_BASE_NAME).tar.gz: ensure-public-key
	rm -rf $(KEYRING_BASE_NAME)
	mkdir -p $(KEYRING_BASE_NAME)
	gpg --armor --export '$(GPG_UID)' > \
	  $(KEYRING_BASE_NAME)/groonga-keyring.gpg
	tar cvzf $(KEYRING_BASE_NAME).tar.gz $(KEYRING_BASE_NAME)
