--source include/not_embedded.inc

--eval SET @global=@@global.$var

--echo # Test that "SET $var" is not allowed without $grant

CREATE USER user1@localhost;
GRANT ALL PRIVILEGES ON *.* TO user1@localhost;
--eval REVOKE $grant ON *.* FROM user1@localhost
--connect(user1,localhost,user1,,)
--connection user1
--error ER_SPECIFIC_ACCESS_DENIED_ERROR
--eval SET GLOBAL $var=$value
--error ER_GLOBAL_VARIABLE
--eval SET $var=$value
--error ER_GLOBAL_VARIABLE
--eval SET SESSION $var=$value
--disconnect user1
--connection default
--disable_warnings
DROP USER user1@localhost;
--enable_warnings

--echo # Test that "SET $var" is allowed with $grant

CREATE USER user1@localhost;
--eval GRANT $grant ON *.* TO user1@localhost
--connect(user1,localhost,user1,,)
--connection user1
--eval SET GLOBAL $var=$value
--error ER_GLOBAL_VARIABLE
--eval SET $var=$value
--error ER_GLOBAL_VARIABLE
--eval SET SESSION $var=$value
--disconnect user1
--connection default
--disable_warnings
DROP USER user1@localhost;
--enable_warnings

--eval SET @@global.$var=@global
