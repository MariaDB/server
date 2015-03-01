--disable_query_log
if ($WINDOWS)
{
--eval CREATE FUNCTION Json_Array RETURNS STRING SONAME 'ha_connect.dll';
--eval CREATE FUNCTION Json_Array_Add RETURNS STRING SONAME 'ha_connect.dll';
--eval CREATE FUNCTION Json_Object RETURNS STRING SONAME 'ha_connect.dll';
--eval CREATE FUNCTION Json_Object_Nonull RETURNS STRING SONAME 'ha_connect.dll';
--eval CREATE FUNCTION Json_Value returns STRING SONAME 'ha_connect.dll';
--eval CREATE AGGREGATE FUNCTION Json_Array_Grp RETURNS STRING SONAME 'ha_connect.dll';
--eval CREATE AGGREGATE FUNCTION Json_Object_Grp RETURNS STRING SONAME 'ha_connect.dll';
}

if (!$WINDOWS)
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

