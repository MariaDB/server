#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "log.h"

#include "print_log.h"

static void vprint_log(int severity,
		       const char *fmt, va_list args)
{
	char* str;

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
		case 0:
			sql_print_information("Plugin daemon_memcached_engine_ib:%s", str);
			break;
		case 1:
			sql_print_warning("Plugin daemon_memcached_engine_ib:%s", str);
			break;
		case 2:
			sql_print_error("Plugin daemon_memcached_engine_ib:%s", str);
			break;
	}

	free(str);
}

void print_log_info(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vprint_log(0, fmt, args);
	va_end(args);
}

void print_log_warning(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vprint_log(1, fmt, args);
	va_end(args);
}

void print_log_error(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vprint_log(2, fmt, args);
	va_end(args);
}
