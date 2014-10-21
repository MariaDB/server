EXTRA_DIST =					\
	configure

CONFIGURE_DEPENDENCIES =			\
	configure

ALL_DEPEND_TARGETS =
INSTALL_DEPEND_TARGETS =
CLEAN_DEPEND_TARGETS =
if WITH_MRUBY
ALL_DEPEND_TARGETS += onigmo-all
if WITH_SHARED_ONIGMO
INSTALL_DEPEND_TARGETS += onigmo-install
endif
CLEAN_DEPEND_TARGETS += onigmo-clean
endif

onigmo-all:
	cd ../onigmo-source && $(MAKE) all

all: $(ALL_DEPEND_TARGETS)

onigmo-install:
	cd ../onigmo-source && $(MAKE) install

install: $(INSTALL_DEPEND_TARGETS)

onigmo-clean:
	cd ../onigmo-source && $(MAKE) clean

clean: $(CLEAN_DEPEND_TARGETS)
