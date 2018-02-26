SUBDIRS =					\
	command_line				\
	context					\
	expression_tree				\
	initialize				\
	logger					\
	query_logger

include sources.am

EXTRA_DIST =					\
	$(RUBY_SCRIPT_FILES)

if WITH_MRUBY
ruby_scripts_DATA = $(RUBY_SCRIPT_FILES)
endif
