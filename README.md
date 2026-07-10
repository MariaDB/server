# MariaDB Server

MariaDB Server is one of the most widely deployed open source relational
 databases. It is built by a global community of contributors together with
the MariaDB Foundation and MariaDB plc, and it powers workloads from small
applications to large-scale production systems.

MariaDB began as a fork of MySQL, led by the original MySQL developers,
and it maintains a high degree of MySQL compatibility. It has since grown
into a database with its own capabilities, including native vector search,
multiple pluggable storage engines, synchronous clustering, and analytical
features, while remaining a straightforward migration target for existing
MySQL deployments.

## Key features

* **Native vector search**. A built-in VECTOR data type with approximate
  nearest-neighbour indexing (HNSW), available since MariaDB 11.8 with
  no extension required.
* **Pluggable storage engines**. InnoDB (default for transactional workloads),
  Aria, MyRocks, ColumnStore for analytics, Spider for sharding, and S3
  for archival, among others.
* **Replication and clustering**. Asynchronous, semi-synchronous, and
  parallel replication with global transaction IDs, plus Galera synchronous
  multi-primary clustering.
* **Advanced SQL**. Common table expressions and recursive CTEs, window
  functions, system-versioned (temporal) tables, sequences, and a broad set
  of JSON functions.
* **MySQL and Oracle compatibility**. Wire-protocol and syntax compatibility
  with MySQL, plus an Oracle SQL mode supporting PL/SQL-style stored routines.
* **Spatial and full-text search**. GIS data types and functions, and built-in
  full-text indexing.
* **Security**. Role-based access control, pluggable authentication (ed25519,
  PAM, GSSAPI, and more), data-at-rest encryption, and TLS for connections.

## Documentation

* [Project home, community, and getting involved](https://mariadb.org)
* [Reference manual and release notes](https://mariadb.com/docs/)
* [MariaDB compared to MySQL](https://mariadb.com/docs/release-notes/community-server/about/compatibility-and-differences/mariadb-vs-mysql-features)

## Releases

MariaDB Server follows a yearly long-term support (LTS) model alongside
quarterly rolling releases. Binary LTS releases are maintained for three years
and are recommended for production; rolling releases deliver new features
sooner with a shorter support window.

For the current version and full history, see
 [Releases](https://github.com/MariaDB/server/releases) and
 the [MariaDB release calendar](https://mariadb.org/mariadb/all-releases/).

## Installing

Packages and installation instructions for all supported platforms are
available at [https://mariadb.org/download/](https://mariadb.org/download/).

## Building from source

To build MariaDB from source and run the test suite, follow the
[developer guide](https://mariadb.org/get-involved/getting-started-for-developers/get-code-build-test/)

It covers building the code correctly, running the MariaDB testing framework,
and choosing the right branch to target for contributions.

## Contributing

Contributions are welcome, from code and documentation to bug reports and
reviews. See [CONTRIBUTING.md](CONTRIBUTING.md) to
get started, and [CODING_STANDARDS.md](CODING_STANDARDS.md)
for code style. Contributors and community members are recognised in
[CREDITS](CREDITS).

## Getting help

* [Zulip chat](https://mariadb.zulipchat.com/)
* [Maria Discuss mailing list](https://lists.mariadb.org/postorius/lists/discuss.lists.mariadb.org/)
* [Bug reports](https://jira.mariadb.org)
* Security vulnerabilities: see [SECURITY.md](SECURITY.md)

## Licensing

MariaDB Server is licensed under version 2 of the GNU General Public
License (GPLv2), without the "any later version" clause. This is inherited from
MySQL. License information is in the [COPYING](COPYING)
file, and third-party license information is in the
[THIRDPARTY](THIRDPARTY) file.

## About MariaDB Foundation

MariaDB is brought to you by the MariaDB Foundation and MariaDB plc.
The MariaDB Foundation is the custodian of the MariaDB Server codebase,
ensuring it stays open and that anyone can contribute.
See [CREDITS](CREDITS) for details on the Foundation
and the people developing MariaDB.

MySQL, the base of MariaDB, is a product and trademark of Oracle Corporation, Inc.
