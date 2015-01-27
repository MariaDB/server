#
# suite.pm will make sure that all tests including this file
# will be skipped unless this is an embedded test run
#
# The test below is redundant

if (`select version() like '%embedded%'`) {
  This should never happen;
}

