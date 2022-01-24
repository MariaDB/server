let $VERSION_COMPILE_OS_WIN=
  `SELECT IF(@@version_compile_os like 'Win%', 1, 0)`;
if ($VERSION_COMPILE_OS_WIN)
{
  INSTALL PLUGIN spider SONAME 'ha_spider';
  if ($MASTER_1_MYPORT)
  {
    evalp CREATE SERVER s_1 FOREIGN DATA WRAPPER mysql OPTIONS (
      HOST 'localhost',
      DATABASE 'auto_test_local',
      USER 'root',
      PASSWORD '',
      PORT $MASTER_1_MYPORT
    );
  }
  if ($CHILD2_1_MYPORT)
  {
    evalp CREATE SERVER s_2_1 FOREIGN DATA WRAPPER mysql OPTIONS (
      HOST 'localhost',
      DATABASE 'auto_test_remote',
      USER 'root',
      PASSWORD '',
      PORT $CHILD2_1_MYPORT
    );
  }
  if ($CHILD2_2_MYPORT)
  {
    evalp CREATE SERVER s_2_2 FOREIGN DATA WRAPPER mysql OPTIONS (
      HOST 'localhost',
      DATABASE 'auto_test_remote2',
      USER 'root',
      PASSWORD '',
      PORT $CHILD2_2_MYPORT
    );
  }
  if ($CHILD2_3_MYPORT)
  {
    evalp CREATE SERVER s_2_3 FOREIGN DATA WRAPPER mysql OPTIONS (
      HOST 'localhost',
      DATABASE 'auto_test_remote3',
      USER 'root',
      PASSWORD '',
      PORT $CHILD2_3_MYPORT
    );
  }
  if ($CHILD3_1_MYPORT)
  {
    evalp CREATE SERVER s_3_1 FOREIGN DATA WRAPPER mysql OPTIONS (
      HOST 'localhost',
      DATABASE 'auto_test_local',
      USER 'root',
      PASSWORD '',
      PORT $CHILD3_1_MYPORT
    );
  }
  if ($CHILD3_2_MYPORT)
  {
    evalp CREATE SERVER s_3_2 FOREIGN DATA WRAPPER mysql OPTIONS (
      HOST 'localhost',
      DATABASE 'auto_test_local',
      USER 'root',
      PASSWORD '',
      PORT $CHILD3_2_MYPORT
    );
  }
  if ($CHILD2_3_MYPORT)
  {
    evalp CREATE SERVER s_3_3 FOREIGN DATA WRAPPER mysql OPTIONS (
      HOST 'localhost',
      DATABASE 'auto_test_local',
      USER 'root',
      PASSWORD '',
      PORT $CHILD2_3_MYPORT
    );
  }
}
if (!$VERSION_COMPILE_OS_WIN)
{
  INSTALL PLUGIN spider SONAME 'ha_spider';
  if ($MASTER_1_MYSOCK)
  {
    evalp CREATE SERVER s_1 FOREIGN DATA WRAPPER mysql OPTIONS (
      HOST 'localhost',
      DATABASE 'auto_test_local',
      USER 'root',
      PASSWORD '',
      SOCKET '$MASTER_1_MYSOCK'
    );
  }
  if ($CHILD2_1_MYSOCK)
  {
    evalp CREATE SERVER s_2_1 FOREIGN DATA WRAPPER mysql OPTIONS (
      HOST 'localhost',
      DATABASE 'auto_test_remote',
      USER 'root',
      PASSWORD '',
      SOCKET '$CHILD2_1_MYSOCK'
    );
  }
  if ($CHILD2_2_MYSOCK)
  {
    evalp CREATE SERVER s_2_2 FOREIGN DATA WRAPPER mysql OPTIONS (
      HOST 'localhost',
      DATABASE 'auto_test_remote2',
      USER 'root',
      PASSWORD '',
      SOCKET '$CHILD2_2_MYSOCK'
    );
  }
  if ($CHILD2_3_MYSOCK)
  {
    evalp CREATE SERVER s_2_3 FOREIGN DATA WRAPPER mysql OPTIONS (
      HOST 'localhost',
      DATABASE 'auto_test_remote3',
      USER 'root',
      PASSWORD '',
      SOCKET '$CHILD2_3_MYSOCK'
    );
  }
  if ($CHILD3_1_MYSOCK)
  {
    evalp CREATE SERVER s_3_1 FOREIGN DATA WRAPPER mysql OPTIONS (
      HOST 'localhost',
      DATABASE 'auto_test_local',
      USER 'root',
      PASSWORD '',
      SOCKET '$CHILD3_1_MYSOCK'
    );
  }
  if ($CHILD3_2_MYSOCK)
  {
    evalp CREATE SERVER s_3_2 FOREIGN DATA WRAPPER mysql OPTIONS (
      HOST 'localhost',
      DATABASE 'auto_test_local',
      USER 'root',
      PASSWORD '',
      SOCKET '$CHILD3_2_MYSOCK'
    );
  }
  if ($CHILD3_3_MYSOCK)
  {
    evalp CREATE SERVER s_3_3 FOREIGN DATA WRAPPER mysql OPTIONS (
      HOST 'localhost',
      DATABASE 'auto_test_local',
      USER 'root',
      PASSWORD '',
      SOCKET '$CHILD3_3_MYSOCK'
    );
  }
}

let $SERVER_NAME=MariaDB;
let $SERVER_MAJOR_VERSION=
  `SELECT SUBSTRING_INDEX(version(), '.', 1)`;
let $SERVER_MINOR_VERSION=
  `SELECT SUBSTRING_INDEX(SUBSTRING_INDEX(version(), '.', 2), '.', -1)`;
let $PLUGIN_VERSION=
  `SELECT SUBSTRING_INDEX(plugin_version, '.', 1)
     FROM information_schema.plugins
    WHERE plugin_name = 'SPIDER'`;
if (`SELECT IF($PLUGIN_VERSION = 3, 1, 0)`)
{
  let $HAS_REWRITE=
    `SELECT IF (STRCMP('$SERVER_NAME', 'MariaDB') = 0,
                IF ($SERVER_MAJOR_VERSION = 10,
                    IF ($SERVER_MINOR_VERSION < 4,  0, 1),
                    IF ($SERVER_MAJOR_VERSION < 10, 0, 1)),
                0)`;
  let $HAS_REWRITE= 0;
  if ($HAS_REWRITE)
  {
    let $PLUGIN_NAME= spider_flush_rewrite_cache;
    let $PLUGIN_EXIST=
      `SELECT COUNT(*) FROM mysql.func WHERE name = '$PLUGIN_NAME'`;
    while (!$PLUGIN_EXIST)
    {
      let $PLUGIN_EXIST=
        `SELECT COUNT(*) FROM mysql.func WHERE name = '$PLUGIN_NAME'`;
    }
  }
}
let $PLUGIN_NAME= spider_flush_table_mon_cache;
let $PLUGIN_EXIST=
  `SELECT COUNT(*) FROM mysql.func WHERE name = '$PLUGIN_NAME'`;
while (!$PLUGIN_EXIST)
{
  let $PLUGIN_EXIST=
    `SELECT COUNT(*) FROM mysql.func WHERE name = '$PLUGIN_NAME'`;
}

SET spider_internal_sql_log_off= 0;
