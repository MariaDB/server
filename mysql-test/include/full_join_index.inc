# Index axis for the FULL JOIN tests.
#
# Under the allidx combination every column of every base table that is
# not already the first column of some index gains a non-unique index on
# itself.  Under noidx nothing happens and the tables keep the indexes
# their CREATE TABLE gave them.  The work itself is in
# full_join_add_indexes.inc, which a test sources after each group of
# CREATE TABLE and INSERT statements.
#
# A combinations file becomes an independent axis only when it is
# attached to a sourced include, and MTR crosses the combinations of each
# sourced include with those of the test's own combinations file.  So
# sourcing this gives every optimizer switch combination one run with the
# extra indexes and one without.  See mtr_cases.pm, get_tags_from_file
# and collect_one_test_case.
#
# Source this file once and once only.  The list of combination files a
# test collects is not deduplicated, so a second source of this file adds
# a second axis of the same two values and doubles the number of runs.
# That is why the work lives in a separate include with no combinations
# file of its own, which is then free to be sourced as often as needed.
