# The goal of including this file is to enable innodb_encrypt_log combinations
# (see include/innodb_encrypt_log.combinations)

--source include/have_innodb.inc
if ($MTR_COMBINATION_CRYPT)
{
--source ../suite/encryption/include/skip_innodb_log_archive.inc
}
