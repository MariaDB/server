REPOSITORIES_PATH = repositories
DISTRIBUTIONS = debian
ARCHITECTURES = i386 amd64
CODE_NAMES = jessie stretch
MYSQL_VARIANTS = 5.5 mariadb-10.0

all:

release: download build sign-packages update-repository sign-repository upload

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
	./sign-packages.sh '$(GPG_UID)' '$(REPOSITORIES_PATH)/' '$(CODE_NAMES)'

update-repository:
	./update-repository.sh '$(PACKAGE_NAME)' '$(REPOSITORIES_PATH)/' \
	  '$(ARCHITECTURES)' '$(CODE_NAMES)'

sign-repository:
	./sign-repository.sh '$(GPG_UID)' '$(REPOSITORIES_PATH)/' '$(CODE_NAMES)'

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

build: build-package-deb

build-package-deb: source env.sh
	vagrant destroy --force
	for variant in $(MYSQL_VARIANTS); do					\
	  cp env.sh tmp/;							\
	  echo "MYSQL_VARIANT=$${variant}" >> tmp/env.sh;			\
	  for architecture in $(ARCHITECTURES); do				\
	    for code_name in $(CODE_NAMES); do					\
	      rm -rf tmp/debian;						\
	      if [ $${variant} = "5.5" -a $${code_name} = "stretch" ]; then	\
	        continue;							\
	      fi;								\
	      if [ $${code_name} = "stretch" ]; then				\
	        cp -rp $(srcdir)/../debian-mariadb-10.0 tmp/debian;		\
	        for f in `find tmp/debian -maxdepth 2 -type f`; do		\
	          RENAMED=`echo $$f | sed 's/10.0/10.1/'`;			\
	          sed -i'' 's/10.0/10.1/g' $${f};				\
	          if [ $${f} = $$RENAMED ]; then				\
	            continue;							\
	          fi;								\
	          mv $${f} $$RENAMED;						\
	        done;								\
	        if [ $${architecture} = "amd64" ]; then				\
                  sed -i'' 's,lib/mysql/,lib/x86_64-linux-gnu/mariadb18/,'	\
	            tmp/debian/mariadb-server-10.1-mroonga.install;		\
		elif [ $${architecture} = "i386" ]; then			\
                  sed -i'' 's,lib/mysql/,lib/i386-linux-gnu/mariadb18/,'	\
	            tmp/debian/mariadb-server-10.1-mroonga.install;		\
	        fi;								\
	      else								\
	        cp -rp $(srcdir)/../debian-$${variant} tmp/debian;		\
	      fi;						\
	      id=debian-$$code_name-$$architecture;		\
	      vagrant up $$id || exit 1;			\
	      vagrant destroy --force $$id;			\
	    done;						\
	  done;							\
	done

source: tmp/$(PACKAGE)-$(VERSION).tar.gz

tmp/$(PACKAGE)-$(VERSION).tar.gz: $(abs_top_builddir)/$(PACKAGE)-$(VERSION).tar.gz
	mkdir -p tmp
	cp $(abs_top_builddir)/$(PACKAGE)-$(VERSION).tar.gz $@
