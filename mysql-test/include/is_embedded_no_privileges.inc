#
# Test that we do not have an embedded server compiled with privileges.
# This is not a regular setup, but we are running this in buildbot for
# testing of embedded + privileges.
# Some funcs_1 test cannot handle this combination, which is why we have this
# file to disable them
#

--source include/is_embedded.inc

let priv=`SELECT privileges FROM information_schema.columns limit 1`;

if (`SELECT "$priv" <> ""`)
{
   --skip Embedded server is compiled with privileges; Test disabled.
}
