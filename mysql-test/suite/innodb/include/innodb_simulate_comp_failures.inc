--echo #
--echo # Testing robustness against random compression failures
--echo #

--source include/not_embedded.inc
--source include/have_innodb.inc

--let $simulate_comp_failures_save = `SELECT @@innodb_simulate_comp_failures`

--disable_query_log
call mtr.add_suppression("InnoDB: Simulating a compression failure for table `test`\\.`t1`");
--enable_query_log

# create the table with compressed pages of size 8K.
CREATE TABLE t1(id INT AUTO_INCREMENT PRIMARY KEY, msg VARCHAR(255), KEY msg_i(msg)) ENGINE=INNODB ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8;

SHOW CREATE TABLE t1;

# percentage of compressions that will be forced to fail
SET GLOBAL innodb_simulate_comp_failures = 25;

--disable_query_log
--disable_result_log

let $num_inserts_ind = $num_inserts;
let $commit_iterations=50;

while ($num_inserts_ind)
{
  let $repeat = `select floor(rand() * 10)`;
  eval INSERT INTO t1(id, msg)
       VALUES ($num_inserts_ind, REPEAT('abcdefghijklmnopqrstuvwxyz', $repeat));
  dec $num_inserts_ind;
}

--enable_query_log
--enable_result_log

COMMIT;
SELECT COUNT(id) FROM t1;

--disable_query_log
--disable_result_log

# do random ops, making sure that some pages will get fragmented and reorganized.
let $num_ops_ind = $num_ops;
let $commit_count= $commit_iterations;

BEGIN;

while($num_ops_ind)
{
  let $idx = `select floor(rand()*$num_inserts)`;
	let $insert_or_update = `select floor(rand()*3)`;

  let $repeat = `select floor(rand() * 9) + 1`;

  let $msg = query_get_value(`select repeat('abcdefghijklmnopqrstuvwxyz', $repeat) as x`, x, 1);

  let $single_or_multi = `select floor(rand()*10)`;

  if ($insert_or_update)
  {
    let $cnt = query_get_value(SELECT COUNT(*) cnt FROM t1 WHERE id=$idx, cnt, 1);

    if ($cnt)
    {
      let $update = `select floor(rand()*2)`;

      if ($update)
      {
        if ($single_or_multi)
        {
          eval UPDATE t1 SET msg=\"$msg\" WHERE id=$idx;
        }

        if (!$single_or_multi)
        {
          eval UPDATE t1 SET msg=\"$msg\" WHERE id >= $idx - 100 AND id <= $idx + 100;
        }

      }

      if (!$update)
      {
        if ($single_or_multi)
        {
          eval INSERT INTO t1(msg, id) VALUES (\"$msg\", $idx) ON DUPLICATE KEY UPDATE msg=VALUES(msg), id = VALUES(id);
        }

        if (!$single_or_multi)
        {
          let $diff = 200;

          while ($diff)
          {
            eval INSERT INTO t1(msg, id) VALUES (\"$msg\", $idx + 100 - $diff) ON DUPLICATE KEY UPDATE msg=VALUES(msg), id=VALUES(id);

            dec $diff;
          }
        }
      }
    }

    if (!$cnt)
    {
      let $null_msg = `select floor(rand()*2)`;

      if ($null_msg)
      {
        eval INSERT INTO t1(id,msg) VALUES ($idx, NULL);
      }

      if (!$null_msg)
      {
        eval INSERT INTO t1(id, msg) VALUES ($idx, \"$msg\");
      }
    }
  }

  if (!$insert_or_update)
  {
    if ($single_or_multi)
    {
      eval DELETE from t1 WHERE id=$idx;
    }

    if (!$single_or_multi)
    {
      eval DELETE from t1 WHERE id >= $idx - 100 AND id <= $idx + 100;
    }
  }

  dec $commit_count;
  if (!$commit_count)
  {
    let $commit_count= $commit_iterations;
    COMMIT;
    BEGIN;
  }

  dec $num_ops_ind;
}

COMMIT;

# final cleanup
DROP TABLE t1;

eval SET GLOBAL innodb_simulate_comp_failures = $simulate_comp_failures_save;

--enable_query_log
