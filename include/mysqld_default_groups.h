const char *load_default_groups[]= {
"mysqld", "server", MYSQL_BASE_VERSION,
"mariadb", MARIADB_BASE_VERSION, MARIADB_MAJOR_VERSION,
"mariadbd", MARIADBD_BASE_VERSION, MARIADBD_MAJOR_VERSION,
"client-server",
#ifdef WITH_WSREP
"galera",
#endif
0, 0};
