-- Copyright (c) 2007, 2013, Oracle and/or its affiliates.
-- 
-- This program is free software; you can redistribute it and/or modify
-- it under the terms of the GNU General Public License as published by
-- the Free Software Foundation; version 2 of the License.
-- 
-- This program is distributed in the hope that it will be useful,
-- but WITHOUT ANY WARRANTY; without even the implied warranty of
-- MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
-- GNU General Public License for more details.
-- 
-- You should have received a copy of the GNU General Public License
-- along with this program; if not, write to the Free Software
-- Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA

--
-- The initial data for system tables of MySQL Server
--

-- When setting up a "cross bootstrap" database (e.g., creating data on a Unix
-- host which will later be included in a Windows zip file), any lines
-- containing "@current_hostname" are filtered out by mysql_install_db.

-- Get the hostname, if the hostname has any wildcard character like "_" or "%" 
-- add escape character in front of wildcard character to convert "_" or "%" to
-- a plain character
SELECT LOWER( REPLACE((SELECT REPLACE(@@hostname,'_','\_')),'%','\%') )INTO @current_hostname;
SELECT '{"access":18446744073709551615}' INTO @all_privileges;
SELECT '{"access":18446744073709551615,"plugin":"mysql_native_password","authentication_string":"invalid","auth_or":[{},{"plugin":"unix_socket"}]}' into @all_with_auth;


-- Fill "global_priv" table with default users allowing root access
-- from local machine if "global_priv" table didn't exist before
CREATE TEMPORARY TABLE tmp_user_nopasswd LIKE global_priv;
CREATE TEMPORARY TABLE tmp_user_socket LIKE global_priv;
-- Classic passwordless root account.
INSERT INTO tmp_user_nopasswd VALUES ('localhost','root',@all_privileges);
REPLACE INTO tmp_user_nopasswd SELECT @current_hostname,'root',@all_privileges FROM dual WHERE @current_hostname != 'localhost';
REPLACE INTO tmp_user_nopasswd VALUES ('127.0.0.1','root',@all_privileges);
REPLACE INTO tmp_user_nopasswd VALUES ('::1','root',@all_privileges);
-- More secure root account using unix socket auth.
INSERT INTO tmp_user_socket VALUES ('localhost', 'root',@all_with_auth);
REPLACE INTO tmp_user_socket VALUES ('localhost',IFNULL(@auth_root_socket, 'root'),@all_with_auth);
IF @auth_root_socket is not null THEN
  IF not exists(select 1 from information_schema.plugins where plugin_name='unix_socket') THEN
     INSTALL SONAME 'auth_socket'; END IF; END IF;

INSERT INTO global_priv SELECT * FROM tmp_user_nopasswd WHERE @had_user_table=0 AND @auth_root_socket IS NULL;
INSERT INTO global_priv SELECT * FROM tmp_user_socket WHERE @had_user_table=0 AND @auth_root_socket IS NOT NULL;

CREATE TEMPORARY TABLE tmp_proxies_priv LIKE proxies_priv;
INSERT INTO tmp_proxies_priv SELECT Host, User, '', '', TRUE, '', now() FROM tmp_user_nopasswd WHERE Host != 'localhost' AND @auth_root_socket IS NULL;
REPLACE INTO tmp_proxies_priv SELECT @current_hostname, IFNULL(@auth_root_socket, 'root'), '', '', TRUE, '', now() FROM DUAL WHERE @current_hostname != 'localhost';
INSERT INTO  proxies_priv SELECT * FROM tmp_proxies_priv WHERE @had_proxies_priv_table=0;
DROP TABLE tmp_user_nopasswd, tmp_user_socket, tmp_proxies_priv;
