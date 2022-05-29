# ==== Purpose ====
# 
# Ensure that the server is running with binlogging on
#
# ==== Usage ====
#
# source include/have_log_bin.inc;

source include/not_embedded.inc;

if (`select not @@log_bin`) {
  skip Test requires: 'have_log_bin';
}
