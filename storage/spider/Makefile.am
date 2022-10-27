HAVE_HANDLERSOCKET = "-DHAVE_HANDLERSOCKET"

pkgplugindir   = $(pkglibdir)/plugin
INCLUDES       = -I$(SPD_MYSQL_INC)$(top_srcdir)/include \
                 -I$(SPD_MYSQL_INC)$(top_srcdir)/regex \
                 -I$(SPD_MYSQL_INC)$(top_srcdir)/sql \
                 -I$(SPD_MYSQL_INC)$(top_srcdir)/extra/yassl/include \
                 -I./hs_client \
                 $(MYSQL_INCLUDE_PATH) \
                 $(HANDLERSOCKET_INCLUDE_PATH)

noinst_HEADERS = ha_spider.h spd_conn.h spd_db_conn.h \
                 spd_db_include.h spd_err.h spd_sys_table.h \
                 spd_table.h spd_trx.h spd_include.h spd_param.h \
                 spd_direct_sql.h spd_udf.h spd_ping_table.h \
                 spd_copy_tables.h spd_malloc.h \
                 spd_db_mysql.h spd_db_handlersocket.h \
                 hs_client/allocator.hpp hs_client/config.hpp \
                 hs_client/mutex.hpp hs_client/string_util.hpp \
                 hs_client/auto_addrinfo.hpp hs_client/escape.hpp \
                 hs_client/socket.hpp hs_client/thread.hpp \
                 hs_client/auto_file.hpp hs_client/fatal.hpp \
                 hs_client/string_buffer.hpp hs_client/util.hpp \
                 hs_client/auto_ptrcontainer.hpp \
                 hs_client/hstcpcli.hpp hs_client/string_ref.hpp \
                 hs_client/hs_compat.h

lib_LTLIBRARIES       = $(PLUGIN_SPIDER_LTLIBRARIES_TARGET)
EXTRA_LTLIBRARIES     = ha_spider.la
pkgplugin_LTLIBRARIES = @plugin_spider_shared_target@
ha_spider_la_LDFLAGS  = -shared -module -rpath $(pkgplugindir)
ha_spider_la_CXXFLAGS = $(AM_CXXFLAGS) -DMYSQL_DYNAMIC_PLUGIN \
                        $(HAVE_HANDLERSOCKET)
ha_spider_la_CFLAGS   = $(AM_CFLAGS) -DMYSQL_DYNAMIC_PLUGIN \
                        $(HAVE_HANDLERSOCKET)
ha_spider_la_SOURCES  = \
                   spd_param.cc \
                   spd_sys_table.cc \
                   spd_trx.cc \
                   spd_db_conn.cc \
                   spd_conn.cc \
                   spd_table.cc \
                   spd_direct_sql.cc \
                   spd_udf.cc \
                   spd_ping_table.cc \
                   spd_copy_tables.cc \
                   spd_i_s.cc \
                   spd_malloc.cc \
                   ha_spider.cc \
                   spd_db_mysql.cc \
                   spd_db_handlersocket.cc \
                   hs_client/config.cpp \
                   hs_client/escape.cpp \
                   hs_client/fatal.cpp \
                   hs_client/hstcpcli.cpp \
                   hs_client/socket.cpp \
                   hs_client/string_util.cpp

lib_LIBRARIES        = $(PLUGIN_SPIDER_LIBRARIES_TARGET)
EXTRA_LIBRARIES      = libspider.a
noinst_LIBRARIES     = @plugin_spider_static_target@
libspider_a_CXXFLAGS = $(AM_CXXFLAGS) $(HAVE_HANDLERSOCKET)
libspider_a_CFLAGS   = $(AM_CFLAGS) $(HAVE_HANDLERSOCKET)
libspider_a_SOURCES  = \
                   spd_param.cc \
                   spd_sys_table.cc \
                   spd_trx.cc \
                   spd_db_conn.cc \
                   spd_conn.cc \
                   spd_table.cc \
                   spd_direct_sql.cc \
                   spd_udf.cc \
                   spd_ping_table.cc \
                   spd_copy_tables.cc \
                   spd_i_s.cc \
                   spd_malloc.cc \
                   ha_spider.cc \
                   spd_db_mysql.cc \
                   spd_db_handlersocket.cc \
                   hs_client/config.cpp \
                   hs_client/escape.cpp \
                   hs_client/fatal.cpp \
                   hs_client/hstcpcli.cpp \
                   hs_client/socket.cpp \
                   hs_client/string_util.cpp

EXTRA_DIST = plug.in
# Don't update the files from bitkeeper
%::SCCS/s.%
