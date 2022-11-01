let $datadir=`select @@datadir`;

DELIMITER $$;
CREATE PROCEDURE debug_show_key_flags()
BEGIN
  IF @debug_key_flags IS TRUE
  THEN
    FLUSH TABLES;
    -- Wrap SET into EXECUTE IMMEDIATE to avoid
    -- parse time "Unknown system variable" errors in release builds.
    EXECUTE IMMEDIATE "SET debug_dbug='+d,key'";
    SELECT * FROM t1 LIMIT 0;
    EXECUTE IMMEDIATE "SET debug_dbug=''";
  END IF;
END;
$$
DELIMITER ;$$


copy_file $table.frm $datadir/test/t1.frm;
copy_file $table.$EXT_DAT $datadir/test/t1.$EXT_DAT;
copy_file $table.$EXT_IDX $datadir/test/t1.$EXT_IDX;
SHOW CREATE TABLE t1;
CHECK TABLE t1 FOR UPGRADE;
DROP TABLE t1;

copy_file $table.frm $datadir/test/t1.frm;
copy_file $table.$EXT_DAT $datadir/test/t1.$EXT_DAT;
copy_file $table.$EXT_IDX $datadir/test/t1.$EXT_IDX;
CALL debug_show_key_flags();
ALTER TABLE t1 MODIFY d DOUBLE DEFAULT 10, ALGORITHM=INSTANT;
CALL debug_show_key_flags();
DROP TABLE t1;

copy_file $table.frm $datadir/test/t1.frm;
copy_file $table.$EXT_DAT $datadir/test/t1.$EXT_DAT;
copy_file $table.$EXT_IDX $datadir/test/t1.$EXT_IDX;
CALL debug_show_key_flags();
ALTER TABLE t1 MODIFY d DOUBLE DEFAULT 10, ALGORITHM=NOCOPY;
CALL debug_show_key_flags();
DROP TABLE t1;

copy_file $table.frm $datadir/test/t1.frm;
copy_file $table.$EXT_DAT $datadir/test/t1.$EXT_DAT;
copy_file $table.$EXT_IDX $datadir/test/t1.$EXT_IDX;
CALL debug_show_key_flags();
REPAIR TABLE t1;
CALL debug_show_key_flags();
DROP TABLE t1;

copy_file $table.frm $datadir/test/t1.frm;
copy_file $table.$EXT_DAT $datadir/test/t1.$EXT_DAT;
copy_file $table.$EXT_IDX $datadir/test/t1.$EXT_IDX;
CALL debug_show_key_flags();
ALTER TABLE t1 FORCE;
CALL debug_show_key_flags();
DROP TABLE t1;

DROP PROCEDURE debug_show_key_flags;
