EXTRA_DIST =					\
	configure

CONFIGURE_DEPENDENCIES =			\
	configure

ALL_DEPEND_TARGETS =
CLEAN_DEPEND_TARGETS =
if WITH_MRUBY
ALL_DEPEND_TARGETS += onigmo-all
CLEAN_DEPEND_TARGETS += onigmo-clean
endif

onigmo-all:
	cd ../onigmo-source && $(MAKE) all

all: $(ALL_DEPEND_TARGETS)

onigmo-clean:
	cd ../onigmo-source && $(MAKE) clean

clean: $(CLEAN_DEPEND_TARGETS)
