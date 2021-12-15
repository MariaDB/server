# This file sets the default character set that most test expects.

# In most cases the variables should match the community server defaults.
# The values should be changed if the default character set for the server
# changes and all tests have been updated to supported the new character set

# The purpose if this file is to allow users to change these defaults
# without having to update all tests.

--disable_query_log
SET @save_character_set_server= @@character_set_server;
SET @save_collation_server= @@collation_server;
set @@character_set_server="latin1";
set @@collation_server="latin1_swedish_ci";
--enable_query_log
