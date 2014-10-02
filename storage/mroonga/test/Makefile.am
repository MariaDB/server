SUBDIRS = unit

TESTS = run-sql-test.sh
TESTS_ENVIRONMENT =				\
	NO_MAKE="yes"

if WITH_CUTTER
TESTS += run-unit-test.sh
TESTS_ENVIRONMENT += CUTTER="$(CUTTER)"
endif

EXTRA_DIST =					\
	run-unit-test.sh			\
	run-sql-test.sh
