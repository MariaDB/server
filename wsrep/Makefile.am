noinst_LIBRARIES = libwsrep.a
libwsrep_a_SOURCES = wsrep_api.h wsrep_loader.c wsrep_dummy.c wsrep_uuid.c wsrep_gtid.c
noinst_PROGRAMS = wsrep_listener
wsrep_listener_SOURCES = wsrep_listener.c
wsrep_listener_LDADD = $(noinst_LIBRARIES)
wsrep_listener_LDFLAGS = -static -ldl

