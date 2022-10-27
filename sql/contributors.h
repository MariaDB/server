#ifndef CONTRIBUTORS_INCLUDED
#define CONTRIBUTORS_INCLUDED

/* Copyright (c) 2006 MySQL AB, 2009 Sun Microsystems, Inc.
   Use is subject to license terms.

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

/* Structure of the name list */

struct show_table_contributors_st {
  const char *name;
  const char *location;
  const char *comment;
};

/*
  Output from "SHOW CONTRIBUTORS"

  Get permission before editing.

  Names should be encoded using UTF-8.

  See also https://mariadb.com/kb/en/log-of-mariadb-contributions/
*/

struct show_table_contributors_st show_table_contributors[]= {
  /* MariaDB foundation sponsors, in contribution, size , time order */
  {"Alibaba Cloud", "https://www.alibabacloud.com/", "Platinum Sponsor of the MariaDB Foundation"},
  {"Tencent Cloud", "https://cloud.tencent.com", "Platinum Sponsor of the MariaDB Foundation"},
  {"Microsoft", "https://microsoft.com/", "Platinum Sponsor of the MariaDB Foundation"},
  {"MariaDB Corporation", "https://mariadb.com", "Founding member, Platinum Sponsor of the MariaDB Foundation"},
  {"ServiceNow", "https://servicenow.com", "Platinum Sponsor of the MariaDB Foundation"},
  {"Visma", "https://visma.com", "Gold Sponsor of the MariaDB Foundation"},
  {"DBS", "https://dbs.com", "Gold Sponsor of the MariaDB Foundation"},
  {"IBM", "https://www.ibm.com", "Gold Sponsor of the MariaDB Foundation"},
  {"Automattic", "https://automattic.com", "Silver Sponsor of the MariaDB Foundation"},
  {"Percona", "https://www.percona.com/", "Sponsor of the MariaDB Foundation"},
  {"Galera Cluster", "https://galeracluster.com", "Sponsor of the MariaDB Foundation"},

  /* Sponsors of important features */
  {"Google", "USA", "Sponsoring encryption, parallel replication and GTID"},
  {"Facebook", "USA", "Sponsoring non-blocking API, LIMIT ROWS EXAMINED etc"},

  /* Individual contributors, names in historical order, newer first */
  {"Ronald Bradford", "Brisbane, Australia", "EFF contribution for UC2006 Auction"},
  {"Sheeri Kritzer", "Boston, Mass. USA", "EFF contribution for UC2006 Auction"},
  {"Mark Shuttleworth", "London, UK.", "EFF contribution for UC2006 Auction"},
  {NULL, NULL, NULL}
};

#endif /* CONTRIBUTORS_INCLUDED */
