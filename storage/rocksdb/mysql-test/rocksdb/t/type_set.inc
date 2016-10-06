#
# SET column type
#

--disable_warnings
DROP TABLE IF EXISTS t1;
--enable_warnings

# Valid values.

eval CREATE TABLE t1 (
  a SET('') $extra_col_opts,
  b SET('test1','test2','test3','test4','test5') $extra_col_opts,
  c SET('01','02','03','04','05','06','07','08','09','10','11','12','13','14','15','16','17','18','19','20','21','22','23','24','25','26','27','28','29','30','31','32','33','34','35','36','37','38','39','40','41','42','43','44','45','46','47','48','49','50''51','52','53','54','55','56','57','58','59','60','61','62','63','64') $extra_col_opts,
  PRIMARY KEY (c)
) ENGINE=rocksdb;

SHOW COLUMNS IN t1;

INSERT INTO t1 (a,b,c) VALUES 
('','test2,test3','01,34,44,,23'),
('',5,2),
(',','test4,test2','');
--sorted_result
SELECT a,b,c FROM t1;

# Out of range values 
# (should produce warnings)

INSERT INTO t1 (a,b,c) VALUES (0,'test6',-1);
--sorted_result
SELECT a,b,c FROM t1;

# Non-unique values in set
# (should produce a warning)
eval ALTER TABLE t1 ADD COLUMN e SET('a','A') $extra_col_opts;

SHOW COLUMNS IN t1;

--error ER_TOO_BIG_SET
eval ALTER TABLE t1 ADD COLUMN f SET('1','2','3','4','5','6','7','8','9','a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p','q','r','s','t','u','v','w','x','y','z',' ','11','12','13','14','15','16','17','18','19','1a','1b','1c','1d','1e','1f','1g','1h','1i','1j','1k','1l','1m','1n','1o','1p','1q','1r','1s','1t','1u','1v','1w','1x','1y','1z','20','21','22','23','24','25','26','27','28','29','2a','2b','2c','2d','2e','2f','2g','2h','2i','2j','2k','2l','2m','2n','2o','2p','2q','2r','2s','2t','2u','2v','2w','2x','2y','2z','30','31','32','33','34','35','36','37','38','39','3a','3b','3c','3d','3e','3f','3g','3h','3i') $extra_col_opts;

# Simple comparison

--sorted_result
SELECT a,b,c,e FROM t1 WHERE FIND_IN_SET('test2',b)>0 OR a != '';

DROP TABLE t1;

