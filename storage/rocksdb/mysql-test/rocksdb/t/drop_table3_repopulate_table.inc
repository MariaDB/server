# Usage:
# let $max = <count>;
# let $table = <table name>;
# --source drop_table_repopulate_table.inc
#
eval DELETE FROM $table;

--disable_query_log
let $i = 1;
while ($i <= $max) {
  let $insert = INSERT INTO $table VALUES ($i, $i, rpad('a', 499, 'b'));
  inc $i;
  eval $insert;
}
--enable_query_log
