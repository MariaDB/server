SUBDIRS =			\
	images			\
	html			\
	munin			\
	init.d			\
	logrotate.d		\
	systemd			\
	scripts

pkgsysconfdir = $(sysconfdir)/$(PACKAGE)
dist_pkgsysconf_DATA =		\
	groonga.conf		\
	synonyms.tsv

httpdconfdir = $(pkgsysconfdir)/httpd
dist_httpdconf_DATA =		\
	groonga-httpd.conf

EXTRA_DIST =					\
	CMakeLists.txt
