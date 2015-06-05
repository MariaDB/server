SUBDIRS =					\
	command_line				\
	context					\
	initialize				\
	logger

include sources.am

EXTRA_DIST =					\
	test/empty.rb				\
	$(RUBY_SCRIPT_FILES)

if WITH_MRUBY
ruby_scripts_DATA = $(RUBY_SCRIPT_FILES)
endif
