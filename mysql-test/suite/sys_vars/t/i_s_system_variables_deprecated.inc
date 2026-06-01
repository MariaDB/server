--echo # MDEV-35369: Add is_deprecated column to system_variables table

--echo #
--echo # All deprecated variables and accompanying replacement (will be NULL if none exist)
--echo #
SELECT VARIABLE_NAME, DEPRECATED_REPLACEMENT
FROM INFORMATION_SCHEMA.SYSTEM_VARIABLES
WHERE IS_DEPRECATED = "YES"
ORDER BY VARIABLE_NAME;

--echo #
--let $assert_text= Non-deprecated variables (IS_DEPRECATED="NO") must have NULL DEPRECATED_REPLACEMENT
--let $assert_cond= [SELECT COUNT(*) AS c FROM INFORMATION_SCHEMA.SYSTEM_VARIABLES WHERE IS_DEPRECATED = "NO" AND DEPRECATED_REPLACEMENT IS NOT NULL, c, 1] = 0
--source include/assert.inc

--echo # end of 13.0 tests
