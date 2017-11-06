#ifndef VTQ_INCLUDED
#define VTQ_INCLUDED

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


/**
   VTQ stands for 'versioning transaction query': InnoDB system table that holds
   transaction IDs, their corresponding times and other transaction-related
   data which is used for transaction order resolution. When versioned table
   marks its records lifetime with transaction IDs, VTQ is used to get their
   actual timestamps. */


enum vtq_field_t
{
  VTQ_ALL = 0,
  VTQ_TRX_ID,
  VTQ_COMMIT_ID,
  VTQ_BEGIN_TS,
  VTQ_COMMIT_TS,
  VTQ_ISO_LEVEL
};

struct vtq_record_t
{
	ulonglong	trx_id;
	ulonglong	commit_id;
	timeval		begin_ts;
	timeval		commit_ts;
	uchar		iso_level;
};

#endif /* VTQ_INCLUDED */
