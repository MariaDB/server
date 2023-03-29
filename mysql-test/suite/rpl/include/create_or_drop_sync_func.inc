# Creates or drops a stored function as a part of debug-sync based
# synchronization mechanism between replication servers.
#
# Parameters:
# $create_or_drop= [create]
# $server_master = [master]
# $server_slave  = [slave]
if (!$create_or_drop)
{
  --let $create_or_drop=create
}

if (`select strcmp('$create_or_drop', 'create') = 0`)
{
	if (!$server_master)
	{
		--let $server_master=master
	}
	if (!$server_slave)
	{
		--let $server_slave=slave
	}

	--connection $server_master
	# Use a stored function to inject a debug_sync into the appropriate THD.
	# The function does nothing on the master, and on the slave it injects the
	# desired debug_sync action(s).
	SET sql_log_bin=0;
	--delimiter ||
	CREATE FUNCTION foo(x INT, d1 VARCHAR(500), d2 VARCHAR(500))
  	RETURNS INT DETERMINISTIC
  	BEGIN
			RETURN x;
  	END
	||
	--delimiter ;
	SET sql_log_bin=1;

	--connection $server_slave

	SET sql_log_bin=0;
	--delimiter ||
	CREATE FUNCTION foo(x INT, d1 VARCHAR(500), d2 VARCHAR(500))
  	RETURNS INT DETERMINISTIC
  	BEGIN
			IF d1 != '' THEN
      	 SET debug_sync = d1;
		  END IF;
    	IF d2 != '' THEN
      	 SET debug_sync = d2;
      END IF;
    RETURN x;
 	END
	||
	--delimiter ;
SET sql_log_bin=1;
}

if (`select strcmp('$create_or_drop', 'drop') = 0`)
{
	if (!$server_slave)
	{
		--let $server_slave=slave=
	}
  if (!$server_master)
	{
		--let $server_master=master
	}
	--connection $server_slave
  SET DEBUG_SYNC='RESET';

  --connection $server_master
  SET DEBUG_SYNC='RESET';
	DROP FUNCTION foo;
}
