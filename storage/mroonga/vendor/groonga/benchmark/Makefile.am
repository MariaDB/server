SUBDIRS =					\
	fixtures				\
	lib

NONEXISTENT_CXX_SOURCE = nonexistent.cpp

if WITH_BENCHMARK
noinst_PROGRAMS =		\
	bench-table-factory	\
	bench-geo-distance	\
	bench-geo-select	\
	bench-ctx-create	\
	bench-query-optimizer	\
	bench-range-select
endif

EXTRA_DIST =					\
	bench-query-optimizer-ddl.grn

AM_CPPFLAGS =			\
	-I$(srcdir)		\
	-I$(srcdir)/lib		\
	-I$(top_srcdir)/include	\
	-I$(top_srcdir)/lib	\
	$(GROONGA_INCLUDEDIR)

AM_CFLAGS =		\
	$(GLIB_CFLAGS)

LIBS =								\
	$(top_builddir)/lib/libgroonga.la			\
	$(top_builddir)/benchmark/lib/libbenchmark.la		\
	$(GLIB_LIBS)

bench_table_factory_SOURCES = bench-table-factory.c
nodist_EXTRA_bench_table_factory_SOURCES = $(NONEXISTENT_CXX_SOURCE)

bench_geo_distance_SOURCES = bench-geo-distance.c
nodist_EXTRA_bench_geo_distance_SOURCES = $(NONEXISTENT_CXX_SOURCE)

bench_geo_select_SOURCES = bench-geo-select.c
nodist_EXTRA_bench_geo_select_SOURCES = $(NONEXISTENT_CXX_SOURCE)

bench_ctx_create_SOURCES = bench-ctx-create.c
nodist_EXTRA_bench_ctx_create_SOURCES = $(NONEXISTENT_CXX_SOURCE)

bench_query_optimizer_SOURCES = bench-query-optimizer.c
nodist_EXTRA_bench_query_optimizer_SOURCES = $(NONEXISTENT_CXX_SOURCE)

bench_range_select_SOURCES = bench-range-select.c
nodist_EXTRA_bench_range_select_SOURCES = $(NONEXISTENT_CXX_SOURCE)

benchmarks =					\
	run-bench-table-factory			\
	run-bench-geo-distance			\
	run-bench-geo-select			\
	run-bench-ctx-create			\
	run-bench-query-optimizer		\
	run-bench-range-select

run-bench-table-factory: bench-table-factory
	@echo $@:
	./bench-table-factory

run-bench-geo-distance: bench-geo-distance
	@echo $@:
	./bench-geo-distance

run-bench-geo-select: bench-geo-select
	@echo $@:
	env					\
	  RUBY="$(RUBY)"			\
	  GROONGA="$(GROONGA)"			\
	  srcdir="$(srcdir)"			\
	  $(srcdir)/bench-geo-select.sh

run-bench-ctx-create: bench-ctx-create
	@echo $@:
	./bench-ctx-create

run-bench-query-optimizer: bench-query-optimizer
	@echo $@:
	@rm -rf tmp/query-optimizer
	@mkdir -p tmp/query-optimizer
	@env							\
	  GRN_RUBY_SCRIPTS_DIR=$(top_srcdir)/lib/mrb/scripts	\
	  ../src/groonga					\
	  --file $(srcdir)/bench-query-optimizer-ddl.grn	\
	  -n tmp/query-optimizer/db > /dev/null
	env							\
	  GRN_RUBY_SCRIPTS_DIR=$(top_srcdir)/lib/mrb/scripts	\
	  ./bench-query-optimizer

run-bench-range-select: bench-range-select
	@echo $@:
	@[ ! -e tmp ] && ln -s /dev/shm tmp || :
	@mkdir -p tmp/range-select
	env							\
	  GRN_RUBY_SCRIPTS_DIR=$(top_srcdir)/lib/mrb/scripts	\
	  ./bench-range-select

benchmark: $(benchmarks)
