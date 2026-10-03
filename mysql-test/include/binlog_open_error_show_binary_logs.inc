#
# SHOW BINARY LOGS, with the file name's numeric suffix masked in addition
# to the size column already masked by show_binary_logs.inc.
#
# The suite deliberately shares a datadir/server with whatever else MTR
# schedules onto the same worker slot when their startup options are
# judged "compatible enough" for a plain restart instead of a full
# reinstall (observed on CI: a leftover master-bin.* from a prior,
# unrelated test bumped the starting file number here to 000003 instead
# of 000001). None of this suite's scenarios depend on the absolute
# number -- only on whether logging is on/off and how many files exist --
# so the number is masked rather than asserted.
#

--replace_regex /master-bin\.[0-9]+/master-bin.NNNNNN/
--source include/show_binary_logs.inc
