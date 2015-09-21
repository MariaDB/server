--disable_query_log
#
# Check if server has support for loading plugins
#
if (`SELECT @@have_dynamic_loading != 'YES'`) {
  --skip UDF requires dynamic loading
}
if (!$HA_CONNECT_SO) {
  --skip Needs a dynamically built ha_connect.so
}

CREATE FUNCTION json_array RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION json_array_add RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION json_object RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION json_object_nonull RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION jsonvalue RETURNS STRING SONAME 'ha_connect';
CREATE AGGREGATE FUNCTION json_array_grp RETURNS STRING SONAME 'ha_connect';
CREATE AGGREGATE FUNCTION json_object_grp RETURNS STRING SONAME 'ha_connect';

--enable_query_log

