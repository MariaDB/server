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
CREATE FUNCTION json_array_add_values RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION json_array_delete RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION json_object RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION json_object_nonull RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION json_object_add RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION json_object_delete RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION json_object_list RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION jsonvalue RETURNS STRING SONAME 'ha_connect';
CREATE AGGREGATE FUNCTION json_array_grp RETURNS STRING SONAME 'ha_connect';
CREATE AGGREGATE FUNCTION json_object_grp RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION json_item_merge RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION json_get_item RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION jsonget_string RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION jsonget_int RETURNS INTEGER SONAME 'ha_connect';
CREATE FUNCTION jsonget_real RETURNS REAL SONAME 'ha_connect';
CREATE FUNCTION jsonlocate RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION json_locate_all RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION json_file RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION jfile_make RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION json_serialize RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION jbin_array RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION jbin_array_add_values RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION jbin_array_add RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION jbin_array_delete RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION jbin_object RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION jbin_object_nonull RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION jbin_object_add RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION jbin_object_delete RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION jbin_object_list RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION jbin_get_item RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION jbin_item_merge RETURNS STRING SONAME 'ha_connect';
CREATE FUNCTION jbin_file RETURNS STRING SONAME 'ha_connect';

--enable_query_log

