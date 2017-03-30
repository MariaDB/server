# threads are removed from:
# - information_schema.processlist
# - performance_schema.threads
# at different times, so we may have to wait a little more
# for the event_scheduler to shutdown
#
let $wait_condition=
  SELECT COUNT(*) = 0 FROM performance_schema.threads
  WHERE name like 'thread/sql/event%';
--source include/wait_condition.inc
