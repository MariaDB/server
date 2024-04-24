# Use innodb for gitd_slave_pos so we do not get "table should be repaired"
# issues.

--disable_connect_log
if (`SELECT @@catalogs = 1`) {
  let $org_connection=$CURRENT_CONNECTION;
  connect(tmp_def_catalog,localhost,root,,def.mysql);
}
--enable_query_log

ALTER TABLE mysql.gtid_slave_pos ENGINE=InnoDB;

if (`SELECT @@catalogs = 1`) {
  connection $org_connection;
  disconnect tmp_def_catalog;
}
--enable_connect_log
