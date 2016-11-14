#ifndef trx0vtq_h
#define trx0vtq_h

/* Copyright (c) 2016, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA */

#include <vtq.h>
#include "trx0types.h"
#include "mem0mem.h"
#include "rem0types.h"

class vtq_query_t
{
public:
	timeval		prev_query;
	bool		backwards;

	vtq_record_t	result;

	const char * cache_result(mem_heap_t* heap, const rec_t* rec);
	const char * cache_result(
		mem_heap_t* heap,
		const rec_t* rec,
		const timeval &_ts_query,
		bool _backwards);
};

#endif // trx0vtq_h
