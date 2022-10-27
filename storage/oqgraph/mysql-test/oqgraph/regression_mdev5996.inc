# Regression test for https://mariadb.atlassian.net/browse/MDEV-5996
--echo Performing OQGraph regression test mdev5996 - using db=`$oqgraph_database_name`, table=`$oqgraph_table_name`

--disable_warnings
use test;
if ($oqgraph_database_name) {
  eval drop database if exists `$oqgraph_database_name` ;
  eval create database `$oqgraph_database_name` ;
  eval use `$oqgraph_database_name` ;
}
eval drop table if exists `$oqgraph_table_name` ;
drop table if exists vvvvvvv_hhhhhhh;
--enable_warnings

eval CREATE TABLE `$oqgraph_table_name` (
  `version` varchar(10) NOT NULL,
  `updateJSON` mediumtext,
  `prevVersion` varchar(10) NOT NULL,
  `nodeID` bigint(20) unsigned NOT NULL DEFAULT '0',
  `prevNodeID` bigint(20) unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`prevNodeID`,`nodeID`),
  KEY `prevVersion` (`prevVersion`) USING BTREE,
  KEY `version` (`version`) USING BTREE,
  KEY `nodeID` (`nodeID`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8;

eval insert into `$oqgraph_table_name` values ( 'x', 'y', 'z', 0, 0);

eval select * from `$oqgraph_table_name`;

eval CREATE TABLE IF NOT EXISTS vvvvvvv_hhhhhhh (
  latch VARCHAR(32) NULL,
  origid BIGINT UNSIGNED NULL,
  destid BIGINT UNSIGNED NULL,
  weight DOUBLE NULL,
  seq BIGINT UNSIGNED NULL,
  linkid BIGINT UNSIGNED NULL,
  KEY (latch, origid, destid) USING HASH,
  KEY (latch, destid, origid) USING HASH
) ENGINE=OQGRAPH
  data_table='$oqgraph_table_name'
  origid='prevNodeID'
 destid='nodeID';

select * from vvvvvvv_hhhhhhh;

eval drop table if exists `$oqgraph_table_name`;
drop table if exists vvvvvvv_hhhhhhh;

if ($oqgraph_database_name) {
  eval drop database if exists `$oqgraph_database_name`;
}

