# threads are added to:
# - information_schema.processlist
# - performance_schema.threads
# at different times, so we may have to wait a little more
# for the event_scheduler to start
#
let $wait_condition=
  SELECT COUNT(*) = 1 FROM performance_schema.threads
  WHERE name LIKE 'thread/sql/event%' AND processlist_command IS NOT NULL;
--source include/wait_condition.inc
