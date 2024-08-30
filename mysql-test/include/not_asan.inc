# This file should only be used with test that finds bugs in ASan that can not
# be overcome. In normal cases one should fix the bug server/test case or in
# the worst case add a (temporary?) suppression in asan.supp or lsan.supp

if (`select count(*) from information_schema.system_variables where variable_name='have_sanitizer' and global_value like "ASAN%"`)
{
--skip Can't be run with ASan
}
