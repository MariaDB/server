# This file sets the default character set that most test expects.

# In most cases the variables should match the community server defaults.
# The values should be changed if the default character set for the server
# changes and all tests have been updated to supported the new character set

# The purpose if this file is to allow users to change these defaults
# without having to update all tests.

--disable_query_log
SET @@character_set_server=@save_character_set_server;
SET @@collation_server=@save_collation_server;
--enable_query_log
