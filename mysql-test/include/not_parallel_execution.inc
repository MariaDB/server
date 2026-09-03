# Skip this test when the server is running with parallel query enabled.
#
# A parallel scan hands the driving table to its workers in chunks and each
# worker joins and ships its own, so the rows of an unordered select arrive in
# whatever order the workers finished rather than in the order the access
# method produced them.
#
# Nearly every test that trips over this wants --sorted_result instead: it is
# only the arrival order that changed, and the test was never about that. This
# file is for the few whose subject *is* the order -- a test that asserts the
# rows come back in key order because the access method promises it. Sorting
# those would hide the very property being asserted, so they are skipped
# instead, and still run in full whenever parallel query is off.
#
# Session as well as global, so a test is skipped both when the server was
# started with --parallel-worker-threads and when the connection has since
# asked for workers itself.

if (`SELECT @@global.parallel_worker_threads > 1 OR
            @@session.parallel_worker_threads > 1`)
{
  --skip Test asserts a row order that a parallel scan does not preserve
}
