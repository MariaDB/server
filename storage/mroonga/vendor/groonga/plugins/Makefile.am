SUBDIRS =					\
	tokenizers				\
	suggest					\
	table					\
	query_expanders				\
	ruby					\
	token_filters				\
	sharding

EXTRA_DIST =					\
	CMakeLists.txt

if WITH_MRUBY
dist_plugins_DATA =				\
	$(ruby_scripts)
endif

include ruby_scripts.am
