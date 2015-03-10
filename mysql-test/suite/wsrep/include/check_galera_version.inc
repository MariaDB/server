# ==== Purpose ====
# Enable tests to check the galera library version.
#
# ==== Usage ====
# --let $galera_version=25.3.6
# source include/check_galera_lib_version.inc;
#
# Parameters:
#   $galera_version
#     Version of the galera library required by the test.
#

--disable_query_log

eval SET @GALERA_VERSION=(SELECT CONCAT('$galera_version', '%'));

if (!`SELECT COUNT(*) FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE
        VARIABLE_NAME LIKE 'wsrep_provider_version' AND
            VARIABLE_VALUE LIKE @GALERA_VERSION`)
{
    skip Test requires Galera library version $galera_version;
}

--enable_query_log
