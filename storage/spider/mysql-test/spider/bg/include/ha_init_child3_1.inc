--let $CHILD3_1_ENGINE_TYPE=Spider
--let $CHILD3_1_ENGINE=ENGINE=Spider
--source ../../include/init_spider.inc
eval INSERT INTO mysql.spider_link_mon_servers
(db_name, table_name, link_id, sid, server, scheme, host, port, socket,
 username, password, ssl_ca, ssl_capath, ssl_cert, ssl_cipher, ssl_key,
 ssl_verify_server_cert, default_file, default_group) VALUES
('%auto_test_local%', '%ta_l%', '%', $CHILD3_1_SERVER_ID, 's_3_1', NULL, NULL,
 NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL, NULL),
('%auto_test_local%', '%ta_l%', '%', $CHILD3_2_SERVER_ID, 's_3_2', NULL, NULL,
 NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL, NULL),
('%auto_test_local%', '%ta_l%', '%', $CHILD3_3_SERVER_ID, 's_3_3', NULL, NULL,
 NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL, NULL);
