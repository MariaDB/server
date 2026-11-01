SELECT Insert_priv FROM mysql.user WHERE Insert_priv='Y' LIMIT 0;
SELECT Insert_priv FROM mysql.user WHERE Insert_priv='Y_' LIMIT 0;
--error ER_CANT_AGGREGATE_2COLLATIONS
SELECT Insert_priv FROM mysql.user WHERE Insert_priv='Y+' LIMIT 0;
