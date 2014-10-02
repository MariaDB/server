NGINX_DIR = nginx-$(NGINX_VERSION)

SUBDIRS =					\
	onigmo					\
	mruby

EXTRA_DIST =					\
	$(NGINX_DIR)				\
	CMakeLists.txt				\
	plugins/CMakeLists.txt

dist-hook:
	GIT_DIR=$(srcdir)/mruby-source/.git git archive --format=tar HEAD | \
	  tar xf - -C $(distdir)/mruby-source
	$(MKDIR_P) $(distdir)/onigmo-source
	GIT_DIR=$(srcdir)/onigmo-source/.git git archive --format=tar HEAD | \
	  tar xf - -C $(distdir)/onigmo-source
