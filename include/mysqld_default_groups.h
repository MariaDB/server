const char *load_default_groups[]= {
#ifdef WITH_NDBCLUSTER_STORAGE_ENGINE
"mysql_cluster",
#endif
"mysqld", "server", MYSQL_BASE_VERSION,
"mariadb", MARIADB_BASE_VERSION,
"client-server",
#ifdef WITH_WSREP
"galera",
#endif
0, 0};
