DROP DATABASE IF EXISTS auto_test_remote2;
DROP DATABASE IF EXISTS auto_test_remote;
set optimizer_switch = @optimizer_switch_backup;
--source ../../include/deinit_vp.inc
--source ../../../../../spider/mysql-test/spider/include/deinit_spider.inc
