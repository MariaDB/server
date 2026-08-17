# Give every column of every base table in the current database a
# non-unique index on itself, unless the column already leads an index.
# Does nothing outside the allidx combination of full_join_index.inc,
# which is the file that declares the axis and must be sourced once by
# any test that sources this one.
#
# Source this after every group of CREATE TABLE and INSERT statements.
# The work is decided by what the current database holds at that moment,
# so a table created later in a section needs its own source after it.
# Sourcing it again over tables that are already covered adds nothing,
# which is what lets a section place it wherever it is convenient.
#
# A non-unique index is the only kind that can be added without knowing
# the data.  A primary key or a unique index would fail on the duplicates
# and the NULLs that many of these sections put in a join column on
# purpose, so those stay written out in the sections that want them.
#
# What this does not reach: a temporary table, which the information
# schema does not report, and a vector column, which is excluded because
# an index on one is a vector index rather than the ordinary index this
# axis is about.
#
# Identifiers are not quoted below because a backtick would end the query
# MTR is reading.  Every table and column name in the FULL JOIN tests is
# a plain identifier, so none needs quoting.

if ($MTR_COMBINATION_ALLIDX)
{
--disable_query_log
--disable_result_log

let $fj_idx_query= select concat('alter table ', c.table_name, ' add index fj_idx_', c.column_name, ' (', c.column_name, ')') from information_schema.columns c join information_schema.tables t on t.table_schema = c.table_schema and t.table_name = c.table_name where c.table_schema = database() and t.table_type = 'BASE TABLE' and c.data_type <> 'vector' and not exists (select 1 from information_schema.statistics s where s.table_schema = c.table_schema and s.table_name = c.table_name and s.column_name = c.column_name and s.seq_in_index = 1) order by c.table_name, c.ordinal_position limit 1;

let $fj_idx_stmt= `$fj_idx_query`;
while ($fj_idx_stmt != '')
{
  eval $fj_idx_stmt;
  let $fj_idx_stmt= `$fj_idx_query`;
}

--enable_result_log
--enable_query_log
}
