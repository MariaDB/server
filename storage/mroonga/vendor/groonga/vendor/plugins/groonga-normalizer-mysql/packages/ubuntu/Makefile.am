CODE_NAMES = precise,trusty,wily,xenial
SOURCE = ../$(PACKAGE)-$(VERSION).tar.gz

all:

ensure-configuration:
	@if test -z "$(LAUNCHPAD_UPLOADER_PGP_KEY)"; then				\
	  echo "--with-launchpad-uploader-pgp-key configure option must be specified.";	\
	  false;									\
	fi
	@if test -z "$(GROONGA_SOURCE_PATH)"; then				 \
	  echo "--with-groonga-source-path configure option must be specified."; \
	  false;								 \
	fi

upload: source ensure-configuration
	$(GROONGA_SOURCE_PATH)/packages/ubuntu/upload.rb	\
	  --package '$(PACKAGE)'				\
	  --version '$(VERSION)'				\
	  --source-archive '$(SOURCE)'				\
	  --code-names '$(CODE_NAMES)'				\
	  --debian-directory '$(srcdir)/../debian/'		\
	  --pgp-sign-key '$(LAUNCHPAD_UPLOADER_PGP_KEY)'

source: $(SOURCE)

$(SOURCE):
	ln -s $(abs_top_builddir)/$(PACKAGE)-$(VERSION).tar.gz $(SOURCE)
