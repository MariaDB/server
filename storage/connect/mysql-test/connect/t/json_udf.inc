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

let $is_win = `select convert(@@version_compile_os using latin1) IN ("Win32","Win64","Windows")`;

if ($is_win)
{
--eval CREATE FUNCTION Json_Array RETURNS STRING SONAME 'ha_connect.dll';
--eval CREATE FUNCTION Json_Array_Add RETURNS STRING SONAME 'ha_connect.dll';
--eval CREATE FUNCTION Json_Object RETURNS STRING SONAME 'ha_connect.dll';
--eval CREATE FUNCTION Json_Object_Nonull RETURNS STRING SONAME 'ha_connect.dll';
--eval CREATE FUNCTION Json_Value returns STRING SONAME 'ha_connect.dll';
--eval CREATE AGGREGATE FUNCTION Json_Array_Grp RETURNS STRING SONAME 'ha_connect.dll';
--eval CREATE AGGREGATE FUNCTION Json_Object_Grp RETURNS STRING SONAME 'ha_connect.dll';
}

if (!$is_win)
{
--eval CREATE FUNCTION Json_Array RETURNS STRING SONAME 'ha_connect.so';
--eval CREATE FUNCTION Json_Array_Add RETURNS STRING SONAME 'ha_connect.so';
--eval CREATE FUNCTION Json_Object RETURNS STRING SONAME 'ha_connect.so';
--eval CREATE FUNCTION Json_Object_Nonull RETURNS STRING SONAME 'ha_connect.so';
--eval CREATE FUNCTION Json_Value returns STRING SONAME 'ha_connect.so';
--eval CREATE AGGREGATE FUNCTION Json_Array_Grp RETURNS STRING SONAME 'ha_connect.so';
--eval CREATE AGGREGATE FUNCTION Json_Object_Grp RETURNS STRING SONAME 'ha_connect.so';
}
--enable_query_log

