#ifndef AUTHORS_INCLUDED
#define AUTHORS_INCLUDED

/* Copyright (c) 2005, 2010, Oracle and/or its affiliates.

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

struct show_table_authors_st {
  const char *name;
  const char *location;
  const char *comment;
};

/*
  Output from "SHOW AUTHORS"

  If you can update it, you get to be in it :)

  Don't be offended if your name is not in here, just add it!

  Active people in the MariaDB are listed first, active people in MySQL
  then, not active last.

  Names should be encoded using UTF-8.

  See also https://mariadb.com/kb/en/log-of-mariadb-contributions/
*/

struct show_table_authors_st show_table_authors[]= {
  /* Active people on MariaDB */
  { "Michael (Monty) Widenius", "Tusby, Finland",
    "Lead developer and main author" },
  { "Sergei Golubchik", "Kerpen, Germany",
    "Architect, Full-text search, precision math, plugin framework, merges etc" },
  { "Igor Babaev", "Bellevue, USA", "Optimizer, keycache, core work"},
  { "Sergey Petrunia", "St. Petersburg, Russia", "Optimizer"},
  { "Oleksandr Byelkin", "Lugansk, Ukraine",
    "Query Cache (4.0), Subqueries (4.1), Views (5.0)" },
  { "Timour Katchaounov", "Sofia , Bulgaria", "Optimizer"},
  { "Kristian Nielsen", "Copenhagen, Denmark",
    "Replication, Async client prototocol, General buildbot stuff" },
  { "Alexander (Bar) Barkov", "Izhevsk, Russia",
    "Unicode and character sets" },
  { "Alexey Botchkov (Holyfoot)", "Izhevsk, Russia",
    "GIS extensions, embedded server, precision math"},
  { "Daniel Bartholomew", "Raleigh, USA", "MariaDB documentation, Buildbot, releases"},
  { "Colin Charles", "Selangor, Malesia", "MariaDB documentation, talks at a LOT of conferences"},
  { "Sergey Vojtovich", "Izhevsk, Russia",
    "initial implementation of plugin architecture, maintained native storage engines (MyISAM, MEMORY, ARCHIVE, etc), rewrite of table cache"},
  { "Vladislav Vaintroub", "Mannheim, Germany", "MariaDB Java connector, new thread pool, Windows optimizations"},
  { "Elena Stepanova", "Sankt Petersburg, Russia", "QA, test cases"},
  { "Georg Richter", "Heidelberg, Germany", "New LGPL C connector, PHP connector"},
  { "Jan Lindström", "Ylämylly, Finland", "Working on InnoDB"},
  { "Lixun Peng", "Hangzhou, China", "Multi Source replication" },
  { "Olivier Bertrand", "Paris, France", "CONNECT storage engine"},
  { "Kentoku Shiba", "Tokyo, Japan", "Spider storage engine, metadata_lock_info Information schema"},
  { "Percona", "CA, USA", "XtraDB, microslow patches, extensions to slow log"},
  { "Vicentiu Ciorbaru", "Bucharest, Romania", "Roles"},
  { "Sudheera Palihakkara", "", "PCRE Regular Expressions" },
  { "Pavel Ivanov", "USA", "Some patches and bug fixes"},
  { "Konstantin Osipov", "Moscow, Russia",
    "Prepared statements (4.1), Cursors (5.0), GET_LOCK (10.0)" },
  { "Ian Gilfillan", "South Africa", "MariaDB documentation"},
  { "Federico Razolli", "Italy", "MariaDB documentation Italian translation"},
  { "Vinchen", "Shenzhen, China", "Instant ADD Column for InnoDB, Spider engine optimization, from Tencent Game DBA Team" },
  { "Willhan", "Shenzhen, China", "Big Column Compression, Spider engine optimization, from Tencent Game DBA Team" },
  { "Anders Karlsson", "Ystad, Sweden", "Replication patch for enforcing triggers on slave"},
  { "Otto Kekäläinen", "Tampere, Finland", "Debian packaging, install/upgrade engineering, QA pipelines, documentation"},
  { "Daniel Black", "Canberra, Australia", "Modernising large page support, systemd, and bug fixes"},

  /* People working on MySQL code base (not NDB) */
  { "Guilhem Bichot", "Bordeaux, France", "Replication (since 4.0)" },
  { "Andrei Elkin", "Espoo, Finland", "Replication" },
  { "Dmitri Lenev", "Moscow, Russia",
    "Time zones support (4.1), Triggers (5.0)" },
  { "Marc Alff", "Denver, CO, USA", "Signal, Resignal, Performance schema" },
  { "Mikael Ronström", "Stockholm, Sweden",
    "NDB Cluster, Partitioning, online alter table" },
  { "Ingo Strüwing", "Berlin, Germany",
    "Bug fixing in MyISAM, Merge tables etc" },
  {"Marko Mäkelä", "Helsinki, Finland", "InnoDB core developer"},

  /* People not active anymore */
  { "David Axmark", "London, England",
    "MySQL founder; Small stuff long time ago, Monty ripped it out!" },
  { "Brian (Krow) Aker", "Seattle, WA, USA",
    "Architecture, archive, blackhole, federated, bunch of little stuff :)" },
  { "Venu Anuganti", "", "Client/server protocol (4.1)" },
  { "Omer BarNir", "Sunnyvale, CA, USA",
    "Testing (sometimes) and general QA stuff" },
  { "John Birrell", "", "Emulation of pthread_mutex() for OS/2" },
  { "Andreas F. Bobak", "", "AGGREGATE extension to user-defined functions" },
  { "Reggie Burnett", "Nashville, TN, USA", "Windows development, Connectors" },
  { "Kent Boortz", "Orebro, Sweden", "Test platform, and general build stuff" },
  { "Tim Bunce", "", "mysqlhotcopy" },
  { "Yves Carlier", "", "mysqlaccess" },
  { "Joshua Chamas", "Cupertino, CA, USA",
    "Concurrent insert, extended date syntax" },
  { "Petr Chardin", "Moscow, Russia",
    "Instance Manager (5.0), Server log tables (5.1)" },
  { "Wei-Jou Chen", "", "Chinese (Big5) character set" },
  { "Albert Chin-A-Young", "",
    "Tru64 port, large file support, better TCP wrappers support" },
  { "Jorge del Conde", "Mexico City, Mexico", "Windows development" },
  { "Antony T. Curtis", "Norwalk, CA, USA",
    "Parser, port to OS/2, storage engines and some random stuff" },
  { "Yuri Dario", "", "OS/2 port" },
  { "Patrick Galbraith", "Sharon, NH", "Federated Engine, mysqlslap" },
  { "Lenz Grimmer", "Hamburg, Germany",
    "Production (build and release) engineering" },
  { "Nikolay Grishakin", "Austin, TX, USA", "Testing - Server" },
  { "Wei He", "", "Chinese (GBK) character set" },
  { "Eric Herman", "Amsterdam, Netherlands", "Bug fixing - federated" },
  { "Andrey Hristov", "Walldorf, Germany", "Event scheduler (5.1)" },
  { "Alexander (Alexi) Ivanov", "St. Petersburg, Russia", "Replication" },
  { "Mattias Jonsson", "Uppsala, Sweden", "Partitioning" },
  { "Alexander (Salle) Keremidarski", "Sofia, Bulgaria",
    "Bug fixing" },
  { "Mats Kindahl", "Storvreta, Sweden", "Replication" },
  { "Serge Kozlov", "Velikie Luki, Russia", "Testing - Cluster" },
  { "Hakan Küçükyılmaz", "Walldorf, Germany", "Testing - Server" },
  { "Matthias Leich", "Berlin, Germany", "Testing - Server" },
  { "Arjen Lentz", "Brisbane, Australia",
    "Documentation (2001-2004), Dutch error messages, LOG2()" },
  { "Marc Liyanage", "", "Created Mac OS X packages" },
  { "Kelly Long", "Denver, CO, USA", "Pool Of Threads" },
  { "Zarko Mocnik", "", "Sorting for Slovenian language" },
  { "Per-Erik Martin", "Uppsala, Sweden", "Stored Procedures (5.0)" },
  { "Alexis Mikhailov", "", "User-defined functions" },
  { "Sinisa Milivojevic", "Larnaca, Cyprus",
    "UNION (4.0), Subqueries in FROM clause (4.1), many other features" },
  { "Jonathan (Jeb) Miller", "Kyle, TX, USA",
    "Testing - Cluster, Replication" },
  { "Elliot Murphy", "Cocoa, FL, USA", "Replication and backup" },
  { "Pekka Nouisiainen", "Stockholm, Sweden",
    "NDB Cluster: BLOB support, character set support, ordered indexes" },
  { "Alexander Nozdrin", "Moscow, Russia",
    "Bug fixing (Stored Procedures, 5.0)" },
  { "Per Eric Olsson", "", "Testing of dynamic record format" },
  { "Jonas Oreland", "Stockholm, Sweden",
    "NDB Cluster, Online Backup, lots of other things" },
  { "Alexander (Sasha) Pachev", "Provo, UT, USA",
    "Statement-based replication, SHOW CREATE TABLE, mysql-bench" },
  { "Irena Pancirov", "", "Port to Windows with Borland compiler" },
  { "Jan Pazdziora", "", "Czech sorting order" },
  { "Benjamin Pflugmann", "",
    "Extended MERGE storage engine to handle INSERT" },
  { "Igor Romanenko", "",
    "mysqldump" },
  { "Tõnu Samuel", "Estonia",
    "VIO interface, other miscellaneous features" },
  { "Carsten Segieth (Pino)", "Fredersdorf, Germany", "Testing - Server"},
  { "Martin Sköld", "Stockholm, Sweden",
    "NDB Cluster: Unique indexes, integration into MySQL" },
  { "Timothy Smith", "Auckland, New Zealand",
    "Dynamic character sets, parts of the build system, libmysqld"},
  { "Miguel Solorzano", "Florianopolis, Santa Catarina, Brazil",
    "Windows development, Windows NT service"},
  { "Punita Srivastava", "Austin, TX, USA", "Testing - Merlin"},
  { "Alexey Stroganov (Ranger)", "Lugansk, Ukraine", "Testing - Benchmarks"},
  { "Magnus Svensson", "Öregrund, Sweden",
    "NDB Cluster: Integration into MySQL, test framework" },
  { "Zeev Suraski", "", "FROM_UNIXTIME(), ENCRYPT()" },
  { "TAMITO", "",
    "The _MB character set macros and UJIS and SJIS character sets" },
  { "Jani Tolonen", "Helsinki, Finland",
    "mysqlimport, extensions to command-line clients, PROCEDURE ANALYSE()" },
  { "Lars Thalmann", "Stockholm, Sweden",
    "Replication and cluster development" },
  { "Tomas Ulin", "Stockholm, Sweden",
    "NDB Cluster: Configuration, installation" },
  { "Gianmassimo Vigazzola", "", "Initial Windows port" },
  { "Sergey Vojtovich", "Izhevsk, Russia", "Plugins infrastructure (5.1)" },
  { "Matt Wagner", "Northfield, MN, USA", "Bug fixing" },
  { "Jim Winstead Jr.", "Los Angeles, CA, USA", "Bug fixing" },
  { "Peter Zaitsev", "Tacoma, WA, USA",
    "SHA1(), AES_ENCRYPT(), AES_DECRYPT(), bug fixing" },
  {"Mark Mark Callaghan", "Texas, USA", "Statistics patches"},
  {NULL, NULL, NULL}
};

#endif /* AUTHORS_INCLUDED */
