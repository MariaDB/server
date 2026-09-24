--disable_query_log
DROP VIEW v_tailoring;
DROP TABLE t_tailoring;
SET @ctype_tailoring_save_client= NULL;
SET @ctype_tailoring_save_results= NULL;
SET @ctype_tailoring_save_collation= NULL;
--enable_query_log
