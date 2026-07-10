--echo #
--echo # OPEN sys_ref_cursor FOR SELECT ... FOR UPDATE
--echo #

SELECT @@autocommit;
SELECT @@transaction_isolation;
SELECT @@default_storage_engine;

CREATE TABLE t1 (
  id INT PRIMARY KEY,
  worker VARCHAR(32) DEFAULT '',
  ts TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6)
);
SHOW CREATE TABLE t1;


CREATE TABLE t2 (a VARCHAR(128)) ENGINE=MEMORY;

DELIMITER /;
CREATE PROCEDURE p2(for_update BOOL, do_fetch BOOL)
BEGIN
  DECLARE c SYS_REFCURSOR;
  DECLARE v INT;
  START TRANSACTION;
  IF for_update THEN
    OPEN c FOR SELECT id FROM t1 WHERE id=0 FOR UPDATE;
  ELSE
    OPEN c FOR SELECT id FROM t1 WHERE id=0;
  END IF;
  IF do_fetch THEN
    FETCH c INTO v;
  END IF;

  -- signal to the other thread that OPEN happened
  INSERT INTO t2 VALUES
  ('The exact value does not matter in t2. Only COUNT(*) matters');

  IF NOT for_update THEN
    -- If FOR UPDATE is not specified then other thread is not locked
    -- Let the other thread finish INSERT.
    DO SLEEP(30); -- This query will be killed by the other thread
  END IF;
  INSERT INTO t1 VALUES (12, 'p2', SYSDATE(6));
  CLOSE c;
  COMMIT;
END;
/
CREATE PROCEDURE p1(for_update BOOL)
BEGIN
  DECLARE v INT;
  DECLARE session_id INT;
  START TRANSACTION;
  IF for_update THEN
    SET v=(SELECT id FROM t1 WHERE id=0 FOR UPDATE);
  ELSE
    SET v=(SELECT id FROM t1 WHERE id=0);
  END IF;
  INSERT INTO t1 VALUES (11, 'p1', SYSDATE(6));
  COMMIT;

  -- Check if the other thread is executing the SLEEP
  -- statement and kill it to avoid waiting
  SET session_id= (SELECT ID FROM INFORMATION_SCHEMA.PROCESSLIST
                   WHERE INFO LIKE '%SLEEP(%)');
  SELECT CONCAT('p1: session_id IS NOT NULL:', session_id IS NOT NULL) AS msg;
  IF session_id IS NOT NULL
  THEN
    KILL QUERY session_id;
  END IF;
END;
/
DELIMITER ;/


let $for_update=0;

while ($for_update < 2)
{
  let $do_fetch=0;
  while ($do_fetch < 2)
  {
    echo ------------ for_update=$for_update  do_fetch=$do_fetch;

    TRUNCATE TABLE t1;
    TRUNCATE TABLE t2;

    # Let's insert in a transaction to be independent from @@autocommit.
    BEGIN;
    INSERT INTO t1 (id) VALUES (0),(1),(2),(3),(4),(5),(6),(7);
    COMMIT;

    connect(con2,localhost,root);
    connection con2;
    send_eval CALL p2($for_update, $do_fetch);
    connection default;
    # wait for the thread con2 to finish the OPEN statement in p2()
    let $wait_condition=SELECT COUNT(*) FROM t2;
    source include/wait_condition.inc;
    eval CALL p1($for_update);
    connection con2;
    reap;
    disconnect con2;
    connection default;

    if (!$for_update)
    {
      echo # Without FOR UPDATE: p1 inserted first;
    }
    if ($for_update)
    {
      echo # With FOR UPDATE: p2 inserted first;
    }

    SELECT id, worker FROM t1 WHERE worker<>'' ORDER BY ts;
    inc $do_fetch;
  }
  inc $for_update;
}

DROP PROCEDURE p1;
DROP PROCEDURE p2;
DROP TABLE t1;
DROP TABLE t2;
