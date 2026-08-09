/* Copyright (c) 2026 Dearsh Oberoi

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

#include "sql_tablesample.h"
#include "sql_class.h"

int Lex_tablesample::fix_tablesample_fields(THD *thd)
{
DBUG_ENTER("Lex_tablesample::fix_tablesample_fields");
DBUG_ASSERT(thd);
bool err= sampling_percentage->fix_fields_if_needed(thd, NULL);
if(err)
    DBUG_RETURN(1);
if (sampling_percentage->const_item())
{
    double d= sampling_percentage->val_real();
    if (d < 0.0 || d > 100.0)
    DBUG_RETURN(1);
    percentage= d / 100.0;

    seed1= (ulong)(my_rnd(&thd->rand) * (double) 0xFFFFFFFFUL);
    seed2= (ulong)(my_rnd(&thd->rand) * (double) 0xFFFFFFFFUL);
}

DBUG_RETURN(0);
}