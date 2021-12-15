/******************************************************
Percona XtraBackup: hot backup tool for InnoDB
(c) 2009-2014 Percona LLC and/or its affiliates
Originally Created 3/3/2009 Yasufumi Kinoshita
Written by Alexey Kopytov, Aleksandr Kuzminsky, Stewart Smith, Vadim Tkachenko,
Yasufumi Kinoshita, Ignacio Nin and Baron Schwartz.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA

*******************************************************

This file incorporates work covered by the following copyright and
permission notice:

   Copyright 2010 Codership Oy <http://www.codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA

*******************************************************/

#include <my_global.h>
#include <my_base.h>
#include <handler.h>
#include <trx0rseg.h>
#include <mysql/service_wsrep.h>

#include "common.h"
#ifdef WITH_WSREP

#include <wsrep_api.h>

/*! Name of file where Galera info is stored on recovery */
#define XB_GALERA_INFO_FILENAME "xtrabackup_galera_info"

/***********************************************************************
Store Galera checkpoint info in the 'xtrabackup_galera_info' file, if that
information is present in the trx system header. Otherwise, do nothing. */
void
xb_write_galera_info(bool incremental_prepare)
/*==================*/
{
	FILE*		fp;
	XID		xid;
	char		uuid_str[40];
	long long	seqno;
	MY_STAT		statinfo;

	/* Do not overwrite existing an existing file to be compatible with
	servers with older server versions */
	if (!incremental_prepare &&
		my_stat(XB_GALERA_INFO_FILENAME, &statinfo, MYF(0)) != NULL) {

		return;
	}

	xid.null();

	if (!trx_rseg_read_wsrep_checkpoint(xid)) {

		return;
	}

	wsrep_uuid_t uuid;
	memcpy(uuid.data, wsrep_xid_uuid(&xid), sizeof(uuid.data));
	if (wsrep_uuid_print(&uuid, uuid_str,
			     sizeof(uuid_str)) < 0) {
		return;
	}

	fp = fopen(XB_GALERA_INFO_FILENAME, "w");
	if (fp == NULL) {

		die(
		    "could not create " XB_GALERA_INFO_FILENAME
		    ", errno = %d\n",
		    errno);
		exit(EXIT_FAILURE);
	}

	seqno = wsrep_xid_seqno(&xid);

	msg("mariabackup: Recovered WSREP position: %s:%lld\n",
	    uuid_str, (long long) seqno);

	if (fprintf(fp, "%s:%lld", uuid_str, (long long) seqno) < 0) {

		die(
		    "could not write to " XB_GALERA_INFO_FILENAME
		    ", errno = %d\n",
		    errno);;
	}

	fclose(fp);
}
#endif
