/* Copyright (C) 2013 Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 or later of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#ifndef _my_rnd_h
#define _my_rnd_h

C_MODE_START

struct my_rnd_struct {
  unsigned long seed1,seed2,max_value;
  double max_value_dbl;
};

void my_rnd_init(struct my_rnd_struct *rand_st, ulong seed1, ulong seed2);
double my_rnd(struct my_rnd_struct *rand_st);
double my_rnd_ssl(struct my_rnd_struct *rand_st);

C_MODE_END

#endif /* _my_rnd_h */
