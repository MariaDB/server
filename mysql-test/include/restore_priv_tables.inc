# Restore the snapshot taken by include/backup_priv_tables.inc
--disable_query_log
DELETE FROM mysql.global_priv;
DELETE FROM mysql.roles_mapping;
DELETE FROM mysql.tables_priv;
DELETE FROM mysql.columns_priv;
DELETE FROM mysql.procs_priv;
DELETE FROM mysql.proxies_priv;
DELETE FROM mysql.db;
INSERT INTO mysql.global_priv   SELECT * FROM backup_global_priv;
INSERT INTO mysql.roles_mapping SELECT * FROM backup_roles_mapping;
INSERT INTO mysql.tables_priv   SELECT * FROM backup_tables_priv;
INSERT INTO mysql.columns_priv  SELECT * FROM backup_columns_priv;
INSERT INTO mysql.procs_priv    SELECT * FROM backup_procs_priv;
INSERT INTO mysql.proxies_priv  SELECT * FROM backup_proxies_priv;
INSERT INTO mysql.db            SELECT * FROM backup_db;
DROP TABLE backup_global_priv, backup_roles_mapping, backup_tables_priv,
  backup_columns_priv, backup_procs_priv, backup_proxies_priv, backup_db;
FLUSH PRIVILEGES;
--enable_query_log
