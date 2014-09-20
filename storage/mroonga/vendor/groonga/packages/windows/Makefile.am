SUBDIRS =					\
	patches					\
	language-files
EXTRA_DIST = Rakefile
LICENSE_DIR = dist-x64/share/license
ARCHITECTURES = x86 x64

all:

release: build package installer upload

ensure-rsync-path:
	@if test -z "$(RSYNC_PATH)"; then				\
	  echo "--with-rsync-path configure option must be specified.";	\
	  false;							\
	fi

download: ensure-rsync-path
	rsync -avz --progress $(RSYNC_PATH)/windows/groonga/ files

upload: ensure-rsync-path
	rsync -avz --progress --delete files/ $(RSYNC_PATH)/windows/groonga

build_options =					\
	VERSION=$(VERSION)			\
	SOURCE=$(SOURCE)			\
	DEBUG_BUILD=$(DEBUG_BUILD)

build: source
	for architecture in $(ARCHITECTURES); do	\
	  $(RUBY) -S rake build $(build_options)	\
	     ARCHITECTURE=$${architecture};		\
	done

build-groonga: source
	for architecture in $(ARCHITECTURES); do		\
	  $(RUBY) -S rake build:groonga $(build_options)	\
	     ARCHITECTURE=$${architecture};			\
	done

LICENSE:
	@(echo "groonga";				\
	 echo "=======";				\
	 echo;						\
	 echo "AUTHORS";				\
	 echo "-------";				\
	 echo;						\
	 cat $(LICENSE_DIR)/groonga/AUTHORS;		\
	 echo;						\
	 echo "COPYING";				\
	 echo "-------";				\
	 echo;						\
	 cat $(LICENSE_DIR)/groonga/COPYING;		\
	 echo;						\
	 echo;						\
	 echo "MeCab";					\
	 echo "=====";					\
	 echo;						\
	 echo "AUTHORS";				\
	 echo "-------";				\
	 echo;						\
	 cat $(LICENSE_DIR)/mecab/AUTHORS;		\
	 echo;						\
	 echo "COPYING";				\
	 echo "-------";				\
	 echo;						\
	 cat $(LICENSE_DIR)/mecab/COPYING;		\
	 echo;						\
	 echo "BSD";					\
	 echo "---";					\
	 echo;						\
	 echo; cat $(LICENSE_DIR)/mecab/BSD;		\
	 echo;						\
	 echo "GPL";					\
	 echo "---";					\
	 echo;						\
	 cat $(LICENSE_DIR)/mecab/GPL;			\
	 echo;						\
	 echo "LGPL";					\
	 echo "----";					\
	 echo;						\
	 cat $(LICENSE_DIR)/mecab/LGPL;			\
	 echo;						\
	 echo;						\
	 echo "NAIST-jdic";				\
	 echo "==========";				\
	 echo;						\
	 echo "AUTHORS";				\
	 echo "-------";				\
	 echo;						\
	 cat $(LICENSE_DIR)/naist-jdic/AUTHORS;		\
	 echo;						\
	 echo "COPYING";				\
	 echo "-------";				\
	 echo;						\
	 cat $(LICENSE_DIR)/naist-jdic/COPYING;		\
	) > LICENSE

installer: installer-x86 installer-x64

setup-x86.nsi: setup-x64.nsi
	sed						\
	  -e 's/64/86/g'				\
	  -e 's/PROGRAMFILES86/PROGRAMFILES32/g'	\
	  setup-x64.nsi > setup-x86.nsi

installer-x86: LICENSE setup-x86.nsi
	makensis setup-x86.nsi

installer-x64: LICENSE setup-x64.nsi
	makensis setup-x64.nsi

package:
	for architecture in $(ARCHITECTURES); do		\
	  mkdir -p files;					\
	  rm -rf files/$(PACKAGE)-$(VERSION)-$${architecture};	\
	  cp -a dist-$${architecture}				\
	    files/$(PACKAGE)-$(VERSION)-$${architecture};	\
	  (cd files &&						\
	    zip -r $(PACKAGE)-$(VERSION)-$${architecture}.zip	\
	           $(PACKAGE)-$(VERSION)-$${architecture});	\
	  rm -rf files/$(PACKAGE)-$(VERSION)-$${architecture};	\
	done

SOURCE=../$(PACKAGE)-$(VERSION).tar.gz

source: $(SOURCE)

$(SOURCE):
	ln -s $(abs_top_builddir)/$(PACKAGE)-$(VERSION).tar.gz ../
