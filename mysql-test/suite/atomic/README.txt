To debug a the ddl_recovery code in a failing ddl_recovery test one could do
_he following:

- Add # before --exec echo "restart" ...
- Force $e (engine), $c (crash point) and $r (crash position) to the values
  where things goes wrong.
- start mariadbd in a debugger

run the following in gdb:
(Replace 'atomic.create_trigger' with the failing test case)

#break ha_recover
#break MYSQL_BIN_LOG::recover
#break MYSQL_BIN_LOG::open

break ddl_log_close_binlogged_events
break ddl_log_execute_action
break ddl_log_execute_recovery()
 run --datadir=/my/maria-10.6/mysql-test/var/log/atomic.create_trigger/mysqld.1/data --log-basename=master --log-bin-index=mysqld-bin.index --debug --log-bin
run
