#pragma once
#include "my_dbug.h"
#ifndef DBUG_OFF
extern char *dbug_mariabackup_get_val(const char *event, const char *key);
/*
In debug mode,  execute SQL statement that was passed via environment.
To use this facility, you need to

1. Add code DBUG_EXECUTE_MARIABACKUP_EVENT("my_event_name", key););
  to the code. key is usually a table name
2. Set environment variable my_event_name_$key SQL statement you want to execute
   when event occurs, in DBUG_EXECUTE_IF from above.
   In mtr , you can set environment via 'let' statement (do not use $ as the first char
   for the variable)
3. start mariabackup with --dbug=+d,debug_mariabackup_events
*/
extern void dbug_mariabackup_event(
	const char *event,const char *key);
#define DBUG_MARIABACKUP_EVENT(A, B) \
	DBUG_EXECUTE_IF("mariabackup_events", \
		dbug_mariabackup_event(A,B););
#define DBUG_EXECUTE_FOR_KEY(EVENT, KEY, CODE) \
	DBUG_EXECUTE_IF("mariabackup_inject_code", {\
		char *dbug_val = dbug_mariabackup_get_val(EVENT, KEY); \
		if (dbug_val && *dbug_val) CODE \
	})
#else
#define DBUG_MARIABACKUP_EVENT(A,B)
#define DBUG_MARIABACKUP_EVENT_LOCK(A,B)
#define DBUG_EXECUTE_FOR_KEY(EVENT, KEY, CODE)
#endif

