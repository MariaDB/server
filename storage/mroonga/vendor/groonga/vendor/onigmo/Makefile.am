EXTRA_DIST =					\
	configure				\
	config.h.cmake				\
	CMakeLists.txt

CONFIGURE_DEPENDENCIES =			\
	configure

if WITH_BUNDLED_ONIGMO
ALL_DEPEND_TARGETS = onigmo-all
CLEAN_DEPEND_TARGETS = onigmo-clean

INSTALL_DEPEND_TARGETS = onigmo-all
if WITH_SHARED_ONIGMO
INSTALL_DEPEND_TARGETS += onigmo-install
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
endif
