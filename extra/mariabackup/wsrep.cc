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

#include <mysql_version.h>
#include <my_base.h>
#include <handler.h>
#include <trx0sys.h>

#include "common.h"
#ifdef WITH_WSREP
#define WSREP_XID_PREFIX "WSREPXid"
#define WSREP_XID_PREFIX_LEN MYSQL_XID_PREFIX_LEN
#define WSREP_XID_UUID_OFFSET 8
#define WSREP_XID_SEQNO_OFFSET (WSREP_XID_UUID_OFFSET + sizeof(wsrep_uuid_t))
#define WSREP_XID_GTRID_LEN (WSREP_XID_SEQNO_OFFSET + sizeof(wsrep_seqno_t))

/*! undefined seqno */
#define WSREP_SEQNO_UNDEFINED (-1)

/*! Name of file where Galera info is stored on recovery */
#define XB_GALERA_INFO_FILENAME "xtrabackup_galera_info"

/* Galera UUID type - for all unique IDs */
typedef struct wsrep_uuid {
    unsigned char data[16];
} wsrep_uuid_t;

/* sequence number of a writeset, etc. */
typedef long long  wsrep_seqno_t;

/* Undefined UUID */
static const wsrep_uuid_t WSREP_UUID_UNDEFINED = {{0,}};

/***********************************************************************//**
Check if a given WSREP XID is valid.

@return true if valid.
*/
static
bool
wsrep_is_wsrep_xid(
/*===============*/
	const void*	xid_ptr)
{
	const XID*	xid = reinterpret_cast<const XID*>(xid_ptr);

	return((xid->formatID      == 1                   &&
		xid->gtrid_length  == WSREP_XID_GTRID_LEN &&
		xid->bqual_length  == 0                   &&
		!memcmp(xid->data, WSREP_XID_PREFIX, WSREP_XID_PREFIX_LEN)));
}

/***********************************************************************//**
Retrieve binary WSREP UUID from XID.

@return binary WSREP UUID represenataion, if UUID is valid, or
	WSREP_UUID_UNDEFINED otherwise.
*/
static
const wsrep_uuid_t*
wsrep_xid_uuid(
/*===========*/
	const XID*	xid)
{
	if (wsrep_is_wsrep_xid(xid)) {
		return(reinterpret_cast<const wsrep_uuid_t*>
		       (xid->data + WSREP_XID_UUID_OFFSET));
	} else {
		return(&WSREP_UUID_UNDEFINED);
	}
}

/***********************************************************************//**
Retrieve WSREP seqno from XID.

@return WSREP seqno, if it is valid, or WSREP_SEQNO_UNDEFINED otherwise.
*/
wsrep_seqno_t wsrep_xid_seqno(
/*==========================*/
	const XID*	xid)
{
	if (wsrep_is_wsrep_xid(xid)) {
		wsrep_seqno_t seqno;
		memcpy(&seqno, xid->data + WSREP_XID_SEQNO_OFFSET,
		       sizeof(wsrep_seqno_t));

		return(seqno);
	} else {
		return(WSREP_SEQNO_UNDEFINED);
	}
}

/***********************************************************************//**
Write UUID to string.

@return length of UUID string representation or -EMSGSIZE if string is too
short.
*/
static
int
wsrep_uuid_print(
/*=============*/
	const wsrep_uuid_t*	uuid,
	char*			str,
	size_t			str_len)
{
	if (str_len > 36) {
		const unsigned char* u = uuid->data;
		return snprintf(str, str_len,
				"%02x%02x%02x%02x-%02x%02x-%02x%02x-"
				"%02x%02x-%02x%02x%02x%02x%02x%02x",
				u[ 0], u[ 1], u[ 2], u[ 3], u[ 4], u[ 5], u[ 6],
				u[ 7], u[ 8], u[ 9], u[10], u[11], u[12], u[13],
				u[14], u[15]);
	}
	else {
		return -EMSGSIZE;
	}
}

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
	wsrep_seqno_t	seqno;
	MY_STAT		statinfo;

	/* Do not overwrite existing an existing file to be compatible with
	servers with older server versions */
	if (!incremental_prepare &&
		my_stat(XB_GALERA_INFO_FILENAME, &statinfo, MYF(0)) != NULL) {

		return;
	}

	xid.null();

	if (!trx_sys_read_wsrep_checkpoint(&xid)) {

		return;
	}

	if (wsrep_uuid_print(wsrep_xid_uuid(&xid), uuid_str,
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
