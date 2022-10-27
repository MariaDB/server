/* Copyright (c) 2008, 2013, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#ifndef TRANSACTION_H
#define TRANSACTION_H

#ifdef USE_PRAGMA_INTERFACE
#pragma interface                      /* gcc class implementation */
#endif

#include <m_string.h>

class THD;

void trans_track_end_trx(THD *thd);

bool trans_begin(THD *thd, uint flags= 0);
bool trans_commit(THD *thd);
bool trans_commit_implicit(THD *thd);
bool trans_rollback(THD *thd);
bool trans_rollback_implicit(THD *thd);

bool trans_commit_stmt(THD *thd);
bool trans_rollback_stmt(THD *thd);

bool trans_savepoint(THD *thd, LEX_CSTRING name);
bool trans_rollback_to_savepoint(THD *thd, LEX_CSTRING name);
bool trans_release_savepoint(THD *thd, LEX_CSTRING name);

void trans_reset_one_shot_chistics(THD *thd);

#endif /* TRANSACTION_H */
