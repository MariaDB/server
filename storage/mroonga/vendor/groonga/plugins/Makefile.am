SUBDIRS =					\
	tokenizers				\
	suggest					\
	query_expanders				\
	ruby					\
	token_filters				\
	sharding				\
	functions				\
	expression_rewriters

EXTRA_DIST =					\
	CMakeLists.txt

if WITH_MRUBY
dist_plugins_DATA =				\
	$(ruby_scripts)
endif

include ruby_scripts.am
