#
# Deinit for check_digest.inc
#

--disable_query_log
drop table test._digests;
drop function test.check_digest;
drop procedure test.reset_digests;
--enable_query_log
