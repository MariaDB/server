# We use this --source include to mark a test as taking long to run.
# We can use this to schedule such test early (to not be left with
# only one or two long tests running, and rests of workers idle), or to
# run a quick test skipping long-running test cases.

--source include/no_valgrind_without_big.inc
