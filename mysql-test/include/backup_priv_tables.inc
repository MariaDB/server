# Snapshot the privilege tables; restore with include/restore_priv_tables.inc
--disable_query_log
CREATE TABLE backup_global_priv   LIKE mysql.global_priv;
CREATE TABLE backup_roles_mapping LIKE mysql.roles_mapping;
CREATE TABLE backup_tables_priv   LIKE mysql.tables_priv;
CREATE TABLE backup_columns_priv  LIKE mysql.columns_priv;
CREATE TABLE backup_procs_priv    LIKE mysql.procs_priv;
CREATE TABLE backup_proxies_priv  LIKE mysql.proxies_priv;
CREATE TABLE backup_db            LIKE mysql.db;
INSERT INTO backup_global_priv   SELECT * FROM mysql.global_priv;
INSERT INTO backup_roles_mapping SELECT * FROM mysql.roles_mapping;
INSERT INTO backup_tables_priv   SELECT * FROM mysql.tables_priv;
INSERT INTO backup_columns_priv  SELECT * FROM mysql.columns_priv;
INSERT INTO backup_procs_priv    SELECT * FROM mysql.procs_priv;
INSERT INTO backup_proxies_priv  SELECT * FROM mysql.proxies_priv;
INSERT INTO backup_db            SELECT * FROM mysql.db;
--enable_query_log
