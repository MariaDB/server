--source include/have_perfschema.inc
--source include/word_size.inc
--vertical_results

# need stable timestamp, because its current value is printed below
set time_zone='+00:00';
set timestamp=unix_timestamp('2014-09-01 13:40:23');
# ditto
set pseudo_thread_id=10;
# to show session != global, and doesn't affect global_value_origin
set sql_mode=ansi_quotes;
# global_value_origin=SQL
set global div_precision_increment=5;

# Don't show values or origin as they may have been changed by the test
# or different recompilation. Remove variables that are depending on
# configuration options

--replace_regex /^\/\S+/PATH/
select VARIABLE_NAME,VARIABLE_SCOPE,VARIABLE_TYPE,VARIABLE_COMMENT,NUMERIC_MIN_VALUE,NUMERIC_MAX_VALUE,NUMERIC_BLOCK_SIZE,ENUM_VALUE_LIST,READ_ONLY,COMMAND_LINE_ARGUMENT
  from information_schema.system_variables
  where variable_name not like 'debug%' and
          variable_name not like 'wsrep%' and
          variable_name not like 's3%' and
          variable_name not in (
          'log_tc_size'
        )
  order by variable_name;

# yet less data: no values, no blocks size, no min/max value.
select VARIABLE_NAME, GLOBAL_VALUE_ORIGIN, VARIABLE_SCOPE, VARIABLE_TYPE, VARIABLE_COMMENT, ENUM_VALUE_LIST, READ_ONLY, COMMAND_LINE_ARGUMENT
  from information_schema.system_variables
  where variable_name in (
          'log_tc_size'
        )
  order by variable_name;

set global div_precision_increment=default;
