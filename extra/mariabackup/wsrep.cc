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
#define MB_GALERA_INFO_FILENAME "mariadb_backup_galera_info"
#define XB_GALERA_DONOR_INFO_FILENAME "donor_galera_info"

/* backup copy of galera info file as sent by donor */
#define MB_GALERA_INFO_FILENAME_SST "mariadb_backup_galera_info_SST"

/***********************************************************************
Store Galera checkpoint info in the MB_GALERA_INFO_FILENAME file, if that
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

	xid.null();

	/* try to read last wsrep XID from innodb rsegs, we will use it
	   instead of galera info file received from donor
	*/
	if (!trx_rseg_read_wsrep_checkpoint(xid)) {
		/* no worries yet, SST may have brought in galera info file
		   from some old MariaDB version, which does not support
		   wsrep XID storing in innodb rsegs
		*/
		return;
	}

	/* if SST brought in galera info file, copy it as *_SST file
	   this will not be used, saved just for future reference
	*/
	if (my_stat(MB_GALERA_INFO_FILENAME, &statinfo, MYF(0))) {
		FILE* fp_in  = fopen(MB_GALERA_INFO_FILENAME, "r");
		FILE* fp_out = fopen(MB_GALERA_INFO_FILENAME_SST, "w");

		char buf[BUFSIZ] = {'\0'};
		size_t size;
		while ((size = fread(buf, 1, BUFSIZ, fp_in))) {
			if (fwrite(buf, 1, size, fp_out) != strlen(buf)) {
				die(
				    "could not write to "
				    MB_GALERA_INFO_FILENAME_SST
				    ", errno = %d\n",
				    errno);
			}
		}
		if (!feof(fp_in)) {
			die(
			    MB_GALERA_INFO_FILENAME_SST
			    " not fully copied\n"
			);
		}
		fclose(fp_out);
		fclose(fp_in);
	}

	wsrep_uuid_t uuid;
	memcpy(uuid.data, wsrep_xid_uuid(&xid), sizeof(uuid.data));
	if (wsrep_uuid_print(&uuid, uuid_str,
			     sizeof(uuid_str)) < 0) {
		return;
	}

	fp = fopen(MB_GALERA_INFO_FILENAME, "w");
	if (fp == NULL) {

		die(
		    "could not create " MB_GALERA_INFO_FILENAME
		    ", errno = %d\n",
		    errno);
	}

	seqno = wsrep_xid_seqno(&xid);

	msg("mariabackup: Recovered WSREP position: %s:%lld domain_id: %lld\n",
	    uuid_str, (long long) seqno, (long long)wsrep_get_domain_id());

	if (fprintf(fp, "%s:%lld %lld", uuid_str, (long long) seqno,
		    (long long)wsrep_get_domain_id()) < 0) {

		die(
		    "could not write to " MB_GALERA_INFO_FILENAME
		    ", errno = %d\n",
		    errno);;
	}

	fclose(fp);
}
#endif
