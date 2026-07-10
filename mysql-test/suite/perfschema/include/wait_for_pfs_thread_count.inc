# Tests for the performance schema
# This helper can be used to enforce that no threads from previous
# tests are still running, which can impact some test scripts.

# Wait until there is only one session left, this one.

let $wait_condition= select count(*) = 1 from performance_schema.threads where `type`='foreground';
--source include/wait_condition.inc

