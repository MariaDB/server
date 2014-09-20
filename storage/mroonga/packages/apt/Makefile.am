REPOSITORIES_PATH = repositories
DISTRIBUTIONS = debian
CHROOT_BASE = /var/lib/chroot
ARCHITECTURES = i386 amd64
CODES = wheezy jessie unstable

all:

release: build sign-packages update-repository sign-repository upload

remove-existing-packages:
	for distribution in $(DISTRIBUTIONS); do		\
	  find $(REPOSITORIES_PATH)/$${distribution}/pool	\
	    -type f -delete;					\
	done

download:
	for distribution in $(DISTRIBUTIONS); do			\
	  rsync -avz --progress --delete				\
	    $(RSYNC_PATH)/$${distribution} $(REPOSITORIES_PATH)/;	\
	done

sign-packages:
	./sign-packages.sh '$(GPG_UID)' '$(REPOSITORIES_PATH)/' '$(CODES)'

update-repository:
	./update-repository.sh '$(PACKAGE_NAME)' '$(REPOSITORIES_PATH)/' \
	  '$(ARCHITECTURES)' '$(CODES)'

sign-repository:
	./sign-repository.sh '$(GPG_UID)' '$(REPOSITORIES_PATH)/' '$(CODES)'

ensure-rsync-path:
	@if test -z "$(RSYNC_PATH)"; then				\
	  echo "--with-rsync-path configure option must be specified.";	\
	  false;							\
	fi

upload: ensure-rsync-path
	for distribution in $(DISTRIBUTIONS); do	\
	  (cd $(REPOSITORIES_PATH)/$${distribution};				\
	   rsync -avz --progress --delete		\
	   dists pool $(RSYNC_PATH)/$${distribution};	\
	  );						\
	done

build: source
	./build-in-chroot.sh \
	  $(PACKAGE) $(VERSION) $(srcdir)/.. $(REPOSITORIES_PATH)/ \
	  $(CHROOT_BASE) '$(ARCHITECTURES)' '$(CODES)'

source: ../$(PACKAGE)-$(VERSION).tar.gz

../$(PACKAGE)-$(VERSION).tar.gz:
	ln -s $(abs_top_builddir)/$(PACKAGE)-$(VERSION).tar.gz ../
