/* Copyright 2018-2018 Codership Oy <http://www.codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#ifndef WSREP_MYSQLD_C_H
#define WSREP_MYSQLD_C_H

enum enum_wsrep_certification_rules {
    WSREP_CERTIFICATION_RULES_STRICT,
    WSREP_CERTIFICATION_RULES_OPTIMIZED
};

/* This is intentionally declared as a weak global symbol, so that
the same ha_innodb.so can be used with the embedded server
(which does not link to the definition of this variable)
and with the regular server built WITH_WSREP. */
extern ulong wsrep_certification_rules __attribute__((weak));

#endif /* WSREP_MYSQLD_C_H */
