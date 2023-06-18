# Export Table and Import from saved files .cfg and .ibd
# Caller should create t1 table definition and populate table

if(!$source_db) {
	let $source_db = test;
}

if(!$dest_db) {
	let $dest_db = test;
}

eval FLUSH TABLES $source_db.t1 FOR EXPORT;

--copy_file $MARIADB_DATADIR/$source_db/t1.cfg $MARIADB_DATADIR/t1.cfg_back
--copy_file $MARIADB_DATADIR/$source_db/t1.ibd $MARIADB_DATADIR/t1.ibd_back

UNLOCK TABLES;

if($source_db != $dest_db) {
	eval USE $dest_db;
	let $create1 = query_get_value(SHOW CREATE TABLE $source_db.t1, Create Table, 1);
	eval $create1;
}

eval ALTER TABLE $dest_db.t1 DISCARD TABLESPACE;

--move_file $MARIADB_DATADIR/t1.cfg_back $MARIADB_DATADIR/$dest_db/t1.cfg
--move_file $MARIADB_DATADIR/t1.ibd_back $MARIADB_DATADIR/$dest_db/t1.ibd

eval ALTER TABLE $dest_db.t1 IMPORT TABLESPACE;

eval CHECK TABLE $dest_db.t1;
eval SHOW CREATE TABLE $dest_db.t1;
eval SELECT * FROM $dest_db.t1;

if($source_db != $dest_db) {
	eval DROP TABLE $dest_db.t1;
}
