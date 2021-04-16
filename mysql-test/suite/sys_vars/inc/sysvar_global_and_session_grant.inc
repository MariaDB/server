--source include/not_embedded.inc


--eval SET @global=@@global.$var

--echo # Test that "SET $var" is not allowed without $grant or SUPER

CREATE USER user1@localhost;
GRANT ALL PRIVILEGES ON *.* TO user1@localhost;
--eval REVOKE $grant, SUPER ON *.* FROM user1@localhost
--connect(user1,localhost,user1,,)
--connection user1
--error ER_SPECIFIC_ACCESS_DENIED_ERROR
--eval SET GLOBAL $var=$value
--error ER_SPECIFIC_ACCESS_DENIED_ERROR
--eval SET $var=$value
--error ER_SPECIFIC_ACCESS_DENIED_ERROR
--eval SET SESSION $var=$value
--disconnect user1
--connection default
DROP USER user1@localhost;

--echo # Test that "SET $var" is allowed with $grant

CREATE USER user1@localhost;
--eval GRANT $grant ON *.* TO user1@localhost
--connect(user1,localhost,user1,,)
--connection user1
--eval SET GLOBAL $var=$value
--eval SET $var=$value
--eval SET SESSION $var=$value
--disconnect user1
--connection default
DROP USER user1@localhost;

--echo # Test that "SET $var" is allowed with SUPER

CREATE USER user1@localhost;
GRANT SUPER ON *.* TO user1@localhost;
--connect(user1,localhost,user1,,)
--connection user1
--eval SET GLOBAL $var=$value
--eval SET $var=$value
--eval SET SESSION $var=$value
--disconnect user1
--connection default
DROP USER user1@localhost;

--eval SET @@global.$var=@global
