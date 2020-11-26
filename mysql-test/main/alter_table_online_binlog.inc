#
# Adds standalone and binlog combinations
#
-- if ($MTR_COMBINATION_BINLOG) {
-- require include/have_log_bin.require
-- }

-- if ($MTR_COMBINATION_STANDALONE) {
-- require include/have_log_bin_off.require
-- }

disable_query_log;
show variables like 'log_bin';
enable_query_log;
