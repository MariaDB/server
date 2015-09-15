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

CREATE FUNCTION Json_Array RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION Json_Array_Add RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION Json_Object RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION Json_Object_Nonull RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION JsonValue RETURNS STRING SONAME 'ha_connect';
CREATE AGGREGATE FUNCTION Json_Array_Grp RETURNS STRING SONAME 'ha_connect';
CREATE AGGREGATE FUNCTION Json_Object_Grp RETURNS STRING SONAME 'ha_connect';

--enable_query_log

