# Drop test_restore, see create_dump_importer.inc.
# The importing connection can outlive its client: wait, or DROP USER warns.
--let $wait_condition= SELECT COUNT(*) = 0 FROM information_schema.processlist WHERE user = 'test_restore'
--source include/wait_condition.inc
DROP USER test_restore;
