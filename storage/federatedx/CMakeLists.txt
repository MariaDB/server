SET(FEDERATEDX_SOURCES ha_federatedx.cc federatedx_txn.cc federatedx_io.cc federatedx_io_null.cc federatedx_io_mysql.cc)
MYSQL_ADD_PLUGIN(federatedx ${FEDERATEDX_SOURCES} STORAGE_ENGINE)
