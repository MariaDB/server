pkgplugindir   = $(pkglibdir)/plugin
INCLUDES       = -I$(MYSQL_INC)$(top_srcdir)/include \
                 -I$(MYSQL_INC)$(top_srcdir)/regex \
                 -I$(MYSQL_INC)$(top_srcdir)/sql \
                 -I$(MYSQL_INC)$(top_srcdir)/extra/yassl/include \
                 $(MYSQL_INCLUDE_PATH)

noinst_HEADERS = ha_vp.h vp_err.h \
                 vp_table.h vp_include.h vp_param.h \
                 vp_udf.h vp_copy_tables.h

lib_LTLIBRARIES       = $(PLUGIN_VP_LTLIBRARIES_TARGET)
EXTRA_LTLIBRARIES     = ha_vp.la
pkgplugin_LTLIBRARIES = @plugin_vp_shared_target@
ha_vp_la_LDFLAGS  = -shared -module -rpath $(pkgplugindir)
ha_vp_la_CXXFLAGS = $(AM_CFLAGS) -DMYSQL_DYNAMIC_PLUGIN
ha_vp_la_CFLAGS   = $(AM_CFLAGS) -DMYSQL_DYNAMIC_PLUGIN
ha_vp_la_SOURCES  = \
                   vp_param.cc \
                   vp_table.cc \
                   vp_udf.cc \
                   vp_copy_tables.cc \
                   ha_vp.cc

lib_LIBRARIES        = $(PLUGIN_VP_LIBRARIES_TARGET)
EXTRA_LIBRARIES      = libvp.a
noinst_LIBRARIES     = @plugin_vp_static_target@
libvp_a_CXXFLAGS = $(AM_CFLAGS)
libvp_a_CFLAGS   = $(AM_CFLAGS)
libvp_a_SOURCES  = \
                   vp_param.cc \
                   vp_table.cc \
                   vp_udf.cc \
                   vp_copy_tables.cc \
                   ha_vp.cc

EXTRA_DIST = plug.in
# Don't update the files from bitkeeper
%::SCCS/s.%
