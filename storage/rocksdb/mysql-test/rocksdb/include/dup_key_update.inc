let $max_table = 2;

--disable_query_log
let $table = 1;
while ($table <= $max_table) {
  let $max = 9;
  let $i = 2;
  while ($i <= $max) {
    let $insert = INSERT INTO t$table VALUES ($i, $i, $i);
    eval $insert;
    inc $i;
  }
  inc $table;
}
--enable_query_log

let $table = 1;
while ($table <= $max_table) {
  let $i = 1;
  let $j = 9;
  while ($i <= $max) {
    
    let $insert = INSERT INTO t$table VALUES ($i, $i, $i) ON DUPLICATE KEY UPDATE id2 = $j;
    eval $insert;

    let $select = SELECT * FROM t$table WHERE id1 = $i;
    eval $select;

    let $select = SELECT * FROM t$table FORCE INDEX (id3) WHERE id3 = $i;
    eval $select;

    inc $j;

    let $insert = INSERT INTO t$table VALUES ($i, $i, $i) ON DUPLICATE KEY UPDATE id2 = $j;
    eval $insert;

    let $select = SELECT * FROM t$table WHERE id1 = $i;
    eval $select;

    let $select = SELECT * FROM t$table FORCE INDEX (id3) WHERE id3 = $i;
    eval $select;

    inc $j;

    let $insert = INSERT INTO t$table VALUES ($i, $i, $i) ON DUPLICATE KEY UPDATE id2 = $j;
    eval $insert;

    let $select = SELECT * FROM t$table WHERE id1 = $i;
    eval $select;

    let $select = SELECT * FROM t$table FORCE INDEX (id3) WHERE id3 = $i;
    eval $select;

    inc $j;

    inc $i;
    inc $i;
    inc $i;
    inc $i;
  }

  let $select = SELECT * FROM t$table;
  eval $select;

  let $select = SELECT * FROM t$table FORCE INDEX (id3);
  eval $select;

  inc $table;
}
