--source include/not_embedded.inc

if (!$AUTH_NAMED_PIPE_SO) {
  skip No auth_named_pipe plugin;
}

if (!$USERNAME) {
  skip USER variable is undefined;
}

if (`SELECT count(*) <> 0 FROM mysql.user WHERE user = '$USERNAME'`) {
  skip %USERNAME%=$USER which exists in mysql.user;
}
