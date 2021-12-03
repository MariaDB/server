#!/bin/sh

# Copyright (C) 2003, 2005 MySQL AB
# Use is subject to license terms
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU Library General Public
# License as published by the Free Software Foundation; version 2
# of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Library General Public License for more details.
#
# You should have received a copy of the GNU Library General Public
# License along with this library; if not, write to the Free
# Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
# MA 02110-1335  USA

#
# postflight - this script will be executed after the MySQL PKG
# installation has been performed.
#
# This script will install the MySQL privilege tables using the
# "mysql_install_db" script and will correct the ownerships of these files
# afterwards.
#

if cd @prefix@ ; then
	if [ ! -f data/mysql/db.frm ] ; then
		./scripts/mysql_install_db --rpm
	fi

	if [ -d data ] ; then
		chown -R @MYSQLD_USER@ data
	fi
else
	exit $?
fi
