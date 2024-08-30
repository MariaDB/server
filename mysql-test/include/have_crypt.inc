# MDEV-27964 The function ENCRYPT() causes SIGSEGV in WITH_MSAN builds
-- source include/not_msan.inc
# encrypt('a') is NULL if crypt(3) is not available
# encrypt('a') is "*0" in fips mode
if (`select length(encrypt('a')) > 3 IS NOT TRUE`) {
  skip No crypt(3);
}
