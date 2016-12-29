#
# This include file creates some basic events which should go to the binary log.
# What happens to the binary log depends on the test which calls the file,
# and should be checked from the test.
#
# Names are intentionally long and ugly, to make grepping more reliable.
#
# Some of events are considered unsafe for SBR (not necessarily correctly, 
# but here isn't the place to check the logic), so we just suppress the warning.
#
# For those few queries which produce result sets (e.g. ANALYZE, CHECKSUM etc.),
# we don't care about the result, so it will not be printed to the output.

call mtr.add_suppression("Unsafe statement written to the binary log using statement format since BINLOG_FORMAT = STATEMENT");

#
# Some DDL
#

CREATE DATABASE database_name_to_encrypt;
USE database_name_to_encrypt;

CREATE USER user_name_to_encrypt;
GRANT ALL ON database_name_to_encrypt.* TO user_name_to_encrypt;
SET PASSWORD FOR user_name_to_encrypt = PASSWORD('password_to_encrypt');

CREATE TABLE innodb_table_name_to_encrypt (
  int_column_name_to_encrypt INT PRIMARY KEY,
  timestamp_column_name_to_encrypt TIMESTAMP(6) NULL,
  blob_column_name_to_encrypt BLOB,
  virt_column_name_to_encrypt INT AS (int_column_name_to_encrypt % 10) VIRTUAL,
  pers_column_name_to_encrypt INT AS (int_column_name_to_encrypt) PERSISTENT,
  INDEX `index_name_to_encrypt`(`timestamp_column_name_to_encrypt`)
) ENGINE=InnoDB 
  PARTITION BY RANGE (int_column_name_to_encrypt)
  SUBPARTITION BY KEY (int_column_name_to_encrypt)
  SUBPARTITIONS 2 (
    PARTITION partition0_name_to_encrypt VALUES LESS THAN (100),
    PARTITION partition1_name_to_encrypt VALUES LESS THAN (MAXVALUE)
  )
;

CREATE TABLE myisam_table_name_to_encrypt (
  int_column_name_to_encrypt INT AUTO_INCREMENT PRIMARY KEY,
  char_column_name_to_encrypt VARCHAR(255),
  datetime_column_name_to_encrypt DATETIME,
  text_column_name_to_encrypt TEXT
) ENGINE=MyISAM;

CREATE TABLE aria_table_name_to_encrypt (
  int_column_name_to_encrypt INT AUTO_INCREMENT PRIMARY KEY,
  varchar_column_name_to_encrypt VARCHAR(1024),
  enum_column_name_to_encrypt ENUM(
    'enum_value1_to_encrypt',
    'enum_value2_to_encrypt'
  ),
  timestamp_column_name_to_encrypt TIMESTAMP(6) NULL,
  blob_column_name_to_encrypt BLOB
) ENGINE=Aria;

CREATE TRIGGER trigger_name_to_encrypt 
  AFTER INSERT ON myisam_table_name_to_encrypt FOR EACH ROW
    INSERT INTO aria_table_name_to_encrypt (varchar_column_name_to_encrypt)
    VALUES (NEW.char_column_name_to_encrypt);

CREATE DEFINER=user_name_to_encrypt VIEW view_name_to_encrypt 
  AS SELECT * FROM innodb_table_name_to_encrypt;

CREATE FUNCTION func_name_to_encrypt (func_parameter_to_encrypt INT)
  RETURNS VARCHAR(64)
  RETURN 'func_result_to_encrypt';
  
--delimiter $$
CREATE PROCEDURE proc_name_to_encrypt (
  IN proc_in_parameter_to_encrypt CHAR(32),
  OUT proc_out_parameter_to_encrypt INT
)
BEGIN
  DECLARE procvar_name_to_encrypt CHAR(64) DEFAULT 'procvar_val_to_encrypt';
  DECLARE cursor_name_to_encrypt CURSOR FOR
    SELECT virt_column_name_to_encrypt FROM innodb_table_name_to_encrypt;
  DECLARE EXIT HANDLER FOR NOT FOUND
  BEGIN
    SET @stmt_var_to_encrypt = CONCAT(
    "SELECT 
      IF (RAND()>0.5,'enum_value2_to_encrypt','enum_value1_to_encrypt')
      FROM innodb_table_name_to_encrypt
      INTO OUTFILE '", proc_in_parameter_to_encrypt, "'");
    PREPARE stmt_to_encrypt FROM @stmt_var_to_encrypt;
    EXECUTE stmt_to_encrypt;
    DEALLOCATE PREPARE stmt_to_encrypt;
  END;
  OPEN cursor_name_to_encrypt;
  proc_label_to_encrypt: LOOP 
    FETCH cursor_name_to_encrypt INTO procvar_name_to_encrypt;
  END LOOP;
  CLOSE cursor_name_to_encrypt;
END $$
--delimiter ;

CREATE SERVER server_name_to_encrypt
  FOREIGN DATA WRAPPER mysql
  OPTIONS (HOST 'host_name_to_encrypt');

--let $_cur_con= $CURRENT_CONNECTION
--connect (con1,localhost,user_name_to_encrypt,password_to_encrypt,database_name_to_encrypt)
CREATE TEMPORARY TABLE tmp_table_name_to_encrypt (
  float_column_name_to_encrypt FLOAT,
  binary_column_name_to_encrypt BINARY(64)
);
--disconnect con1
--connection $_cur_con

CREATE INDEX index_name_to_encrypt 
  ON myisam_table_name_to_encrypt (datetime_column_name_to_encrypt);

ALTER DATABASE database_name_to_encrypt CHARACTER SET utf8;

ALTER TABLE innodb_table_name_to_encrypt 
  MODIFY timestamp_column_name_to_encrypt TIMESTAMP NOT NULL 
  DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
;

ALTER ALGORITHM=MERGE VIEW view_name_to_encrypt 
  AS SELECT * FROM innodb_table_name_to_encrypt;

RENAME TABLE innodb_table_name_to_encrypt TO new_table_name_to_encrypt;
ALTER TABLE new_table_name_to_encrypt RENAME TO innodb_table_name_to_encrypt;

#
# Some DML
#

--disable_warnings

set @user_var1_to_encrypt= 'dyncol1_val_to_encrypt';
set @user_var2_to_encrypt= 'dyncol2_name_to_encrypt';

INSERT INTO view_name_to_encrypt VALUES
  (1, NOW(6), COLUMN_CREATE('dyncol1_name_to_encrypt',@user_var1_to_encrypt), NULL, NULL),
  (2, NOW(6), COLUMN_CREATE(@user_var2_to_encrypt,'dyncol2_val_to_encrypt'), NULL, NULL)
;
--delimiter $$
BEGIN NOT ATOMIC
  DECLARE counter_name_to_encrypt INT DEFAULT 0;
  START TRANSACTION;
  WHILE counter_name_to_encrypt<12 DO
    SELECT COUNT(*) INTO @cnt FROM innodb_table_name_to_encrypt;
    INSERT INTO innodb_table_name_to_encrypt 
      SELECT int_column_name_to_encrypt+@cnt, NOW(6), blob_column_name_to_encrypt, NULL, NULL
      FROM innodb_table_name_to_encrypt
      ORDER BY int_column_name_to_encrypt;
    SET counter_name_to_encrypt = counter_name_to_encrypt+1;
  END WHILE;
  COMMIT;
  END
$$
--delimiter ;

INSERT INTO myisam_table_name_to_encrypt
  SELECT NULL, 'char_literal_to_encrypt', NULL, 'text_to_encrypt';
INSERT INTO myisam_table_name_to_encrypt (char_column_name_to_encrypt) 
  SELECT char_column_name_to_encrypt FROM myisam_table_name_to_encrypt;
INSERT INTO myisam_table_name_to_encrypt (char_column_name_to_encrypt) 
  SELECT char_column_name_to_encrypt FROM myisam_table_name_to_encrypt;
INSERT INTO myisam_table_name_to_encrypt (char_column_name_to_encrypt) 
  SELECT char_column_name_to_encrypt FROM myisam_table_name_to_encrypt;

CALL proc_name_to_encrypt('file_name_to_encrypt',@useless_var_to_encrypt);

TRUNCATE TABLE aria_table_name_to_encrypt;

LOAD DATA INFILE 'file_name_to_encrypt' INTO TABLE aria_table_name_to_encrypt
  (enum_column_name_to_encrypt);

--let datadir= `SELECT @@datadir`
--replace_result $datadir <DATADIR>
eval LOAD DATA LOCAL INFILE '$datadir/database_name_to_encrypt/file_name_to_encrypt' 
  INTO TABLE aria_table_name_to_encrypt (enum_column_name_to_encrypt);
--remove_file $datadir/database_name_to_encrypt/file_name_to_encrypt

UPDATE view_name_to_encrypt SET blob_column_name_to_encrypt = 
  COLUMN_CREATE('dyncol1_name_to_encrypt',func_name_to_encrypt(0))
;

DELETE FROM aria_table_name_to_encrypt ORDER BY int_column_name_to_encrypt LIMIT 10;

--enable_warnings

#
# Other statements
#

--disable_result_log
ANALYZE TABLE myisam_table_name_to_encrypt;
CHECK TABLE aria_table_name_to_encrypt;
CHECKSUM TABLE innodb_table_name_to_encrypt, myisam_table_name_to_encrypt;
--enable_result_log
RENAME USER user_name_to_encrypt to new_user_name_to_encrypt;
REVOKE ALL PRIVILEGES, GRANT OPTION FROM new_user_name_to_encrypt;

#
# Cleanup
#

DROP DATABASE database_name_to_encrypt;
DROP USER new_user_name_to_encrypt;
DROP SERVER server_name_to_encrypt;
