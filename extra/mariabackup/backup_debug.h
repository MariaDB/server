#pragma once
#include "my_dbug.h"
#ifndef DBUG_OFF
char *dbug_mariabackup_get_val(const char *event, fil_space_t::name_type key);
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
#define DBUG_EXECUTE_FOR_KEY(EVENT, KEY, CODE)			\
	DBUG_EXECUTE_IF("mariabackup_inject_code",		\
	{ char *dbug_val= dbug_mariabackup_get_val(EVENT, KEY);	\
	  if (dbug_val) CODE })
#else
#define DBUG_EXECUTE_FOR_KEY(EVENT, KEY, CODE)
#endif

