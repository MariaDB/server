/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <memcached/extension.h>
#include <memcached/sql_print_logger.h>
#include <memcached/engine.h>

#include "log.h"

static EXTENSION_LOG_LEVEL current_log_level = EXTENSION_LOG_WARNING;
SERVER_HANDLE_V1 *sapi;

static const char *sql_print_get_name(void) {
    return "sql_print";
}

static void sql_print_logger_log(EXTENSION_LOG_LEVEL severity,
                                const void* client_cookie,
                                const char *fmt, ...)
{
    char* str;

    if (severity >= current_log_level) {

        (void)client_cookie;

        va_list args;
        va_start(args, fmt);

#if HAVE_VASPRINTF
        if (vasprintf(&str, fmt, args) == -1) {
            /* In case of failure use a fixed length string */
            str = (char *) malloc(BUFSIZ);
            my_vsnprintf(str, BUFSIZ, fmt, args);
        }
#else
        /* Use a fixed length string. */
        str = (char *) malloc(BUFSIZ);
        my_vsnprintf(str, BUFSIZ, fmt, args);
#endif

        int len = strlen(str);
        if (len > 0 && str[len - 1] == '\n') {
            str[len - 1] = '\0';
        }

        switch(severity) {
            case EXTENSION_LOG_DETAIL:
                sql_print_information("Plugin daemon_memcached: %s", str);
                break;
            case EXTENSION_LOG_DEBUG:
                sql_print_information("Plugin daemon_memcached: %s", str);
                break;
            case EXTENSION_LOG_INFO:
                sql_print_information("Plugin daemon_memcached: %s", str);
                break;
            case EXTENSION_LOG_WARNING:
                sql_print_warning("Plugin daemon_memcached: %s", str);
                break;
        }

        va_end(args);
        free(str);

    }
}

static EXTENSION_LOGGER_DESCRIPTOR sql_print_logger_descriptor = {
    .get_name = sql_print_get_name,
    .log = sql_print_logger_log
};

static void on_log_level(const void *cookie,
                         ENGINE_EVENT_TYPE type,
                         const void *event_data,
                         const void *cb_data) {
    if (sapi != NULL) {
        current_log_level = sapi->log->get_level();
    }
}

EXTENSION_ERROR_CODE memcached_initialize_sql_print_logger(GET_SERVER_API get_server_api) {
    sapi = get_server_api();
    if (sapi == NULL) {
        return EXTENSION_FATAL;
    }

    current_log_level = sapi->log->get_level();
    sapi->callback->register_callback(NULL, ON_LOG_LEVEL,
                                      on_log_level, NULL);

    return EXTENSION_SUCCESS;
}

EXTENSION_LOGGER_DESCRIPTOR* get_sql_print_logger(void) {
    return &sql_print_logger_descriptor;
}
