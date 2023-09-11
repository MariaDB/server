--source include/have_compress.inc

# Test that the system is using the default/standard bzip library.
# If not, we have to skip the test as the compression lengths displayed
# in the test will not match the results from used compression library.

if (`select length(COMPRESS(space(5000))) != 33`) {
   skip Test skipped as standard bzip is needed;
}
