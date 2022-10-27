--TEST--
groonga connection
--DESCRIPTION--
required: variables_order includes "E"
--SKIPIF--
<?php
if (!extension_loaded("groonga")) print "skip";
if (!getenv('GROONGA_TEST_HOST')) print "skip";
?>
--FILE--
<?php
$host = getenv('GROONGA_TEST_HOST');
$port = getenv('GROONGA_TEST_PORT') ? getenv('GROONGA_TEST_PORT') : 10041;

$grn = grn_ctx_init();
grn_ctx_connect($grn, $host, $port) or die("cannot connect groong server ({$host}:{$port})");
grn_ctx_send($grn, 'table_list');
var_dump(grn_ctx_recv($grn));
grn_ctx_close($grn);
--EXPECTF--
array(1) {
  [0]=>
  array(2) {
    [0]=>
    int(0)
    [1]=>
    string(%d) "%s"
  }
}
