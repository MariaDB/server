#
# Extract base_port from galera node.
#

# Convert "... base_port = N; ..." to "N; ..."
--let $s1 = `SELECT SUBSTR(@@wsrep_provider_options, LOCATE('base_port =', @@wsrep_provider_options) + LENGTH('base_port = '))`
# Convert "N; ..." to "N"
--let $_NODE_GALERAPORT = `SELECT SUBSTR('$s1', 1, LOCATE(';', '$s1') - 1)`
