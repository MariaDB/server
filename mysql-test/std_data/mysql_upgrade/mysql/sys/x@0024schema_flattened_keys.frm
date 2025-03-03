TYPE=VIEW
query=select `information_schema`.`STATISTICS`.`TABLE_SCHEMA` AS `table_schema`,`information_schema`.`STATISTICS`.`TABLE_NAME` AS `table_name`,`information_schema`.`STATISTICS`.`INDEX_NAME` AS `index_name`,max(`information_schema`.`STATISTICS`.`NON_UNIQUE`) AS `non_unique`,max(if(isnull(`information_schema`.`STATISTICS`.`SUB_PART`),0,1)) AS `subpart_exists`,group_concat(`information_schema`.`STATISTICS`.`COLUMN_NAME` order by `information_schema`.`STATISTICS`.`SEQ_IN_INDEX` ASC separator \',\') AS `index_columns` from `INFORMATION_SCHEMA`.`STATISTICS` where ((`information_schema`.`STATISTICS`.`INDEX_TYPE` = \'BTREE\') and (`information_schema`.`STATISTICS`.`TABLE_SCHEMA` not in (\'mysql\',\'sys\',\'INFORMATION_SCHEMA\',\'PERFORMANCE_SCHEMA\'))) group by `information_schema`.`STATISTICS`.`TABLE_SCHEMA`,`information_schema`.`STATISTICS`.`TABLE_NAME`,`information_schema`.`STATISTICS`.`INDEX_NAME`
md5=f9b8ea559fd673333fbc207077025826
updatable=0
algorithm=1
definer_user=mysql.sys
definer_host=localhost
suid=0
with_check_option=0
timestamp=2024-03-28 05:18:00
create-version=1
source=SELECT TABLE_SCHEMA, TABLE_NAME, INDEX_NAME, MAX(NON_UNIQUE) AS non_unique, MAX(IF(SUB_PART IS NULL, 0, 1)) AS subpart_exists, GROUP_CONCAT(COLUMN_NAME ORDER BY SEQ_IN_INDEX) AS index_columns FROM INFORMATION_SCHEMA.STATISTICS WHERE INDEX_TYPE=\'BTREE\' AND TABLE_SCHEMA NOT IN (\'mysql\', \'sys\', \'INFORMATION_SCHEMA\', \'PERFORMANCE_SCHEMA\') GROUP BY TABLE_SCHEMA, TABLE_NAME, INDEX_NAME
client_cs_name=utf8
connection_cl_name=utf8_general_ci
view_body_utf8=select `information_schema`.`STATISTICS`.`TABLE_SCHEMA` AS `table_schema`,`information_schema`.`STATISTICS`.`TABLE_NAME` AS `table_name`,`information_schema`.`STATISTICS`.`INDEX_NAME` AS `index_name`,max(`information_schema`.`STATISTICS`.`NON_UNIQUE`) AS `non_unique`,max(if(isnull(`information_schema`.`STATISTICS`.`SUB_PART`),0,1)) AS `subpart_exists`,group_concat(`information_schema`.`STATISTICS`.`COLUMN_NAME` order by `information_schema`.`STATISTICS`.`SEQ_IN_INDEX` ASC separator \',\') AS `index_columns` from `INFORMATION_SCHEMA`.`STATISTICS` where ((`information_schema`.`STATISTICS`.`INDEX_TYPE` = \'BTREE\') and (`information_schema`.`STATISTICS`.`TABLE_SCHEMA` not in (\'mysql\',\'sys\',\'INFORMATION_SCHEMA\',\'PERFORMANCE_SCHEMA\'))) group by `information_schema`.`STATISTICS`.`TABLE_SCHEMA`,`information_schema`.`STATISTICS`.`TABLE_NAME`,`information_schema`.`STATISTICS`.`INDEX_NAME`
