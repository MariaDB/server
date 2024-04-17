
// vim:sw=2:ai

/*
 * Copyright (C) 2010-2011 DeNA Co.,Ltd.. All rights reserved.
 * Copyright (C) 2011-2017 Kentoku SHIBA
 * See COPYRIGHT.txt for details.
 */

#ifndef DENA_FATAL_HPP
#define DENA_FATAL_HPP

#include "mysql_version.h"
#include "sql_priv.h"
#include "probes_mysql.h"
#include "sql_class.h"

namespace dena {

void fatal_abort(const String& message);
void fatal_abort(const char *message);

};

#endif

