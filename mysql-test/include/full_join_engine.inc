# Storage engine axis for the FULL JOIN tests.
#
# The engine is chosen entirely by the default-storage-engine option in
# full_join_engine.combinations, so nothing has to happen here.  The file
# exists because a combinations file becomes an independent axis only when
# it is attached to a sourced include.  MTR crosses the combinations of
# each sourced include with those of the test's own combinations file, so
# sourcing this gives every optimizer switch combination one run per
# engine.  See mtr_cases.pm, get_tags_from_file and collect_one_test_case.
#
# A create table that names its own engine is unaffected, which is what
# keeps the mixed engine section meaningful under every combination here.
