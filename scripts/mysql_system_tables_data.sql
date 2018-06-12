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
-- Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

--
-- The inital data for system tables of MySQL Server
--

-- When setting up a "cross bootstrap" database (e.g., creating data on a Unix
-- host which will later be included in a Windows zip file), any lines
-- containing "@current_hostname" are filtered out by mysql_install_db.

-- Get the hostname, if the hostname has any wildcard character like "_" or "%" 
-- add escape character in front of wildcard character to convert "_" or "%" to
-- a plain character
SELECT LOWER( REPLACE((SELECT REPLACE(@@hostname,'_','\_')),'%','\%') )INTO @current_hostname;

-- Fill "reverse_db" table with default denies for anyone to
-- access database 'test', 'test1' and 'test2_%' if "reverse_db" table didn't exist
CREATE TEMPORARY TABLE tmp_reverse_db LIKE db;
INSERT INTO tmp_reverse_db VALUES ('%','test','','N','N','N','Y','N','Y','N','Y','Y','Y','Y','Y','Y','Y','Y','N','N','Y','Y','Y');
INSERT INTO tmp_reverse_db VALUES ('%','test1','','N','Y','N','Y','N','Y','N','Y','Y','Y','Y','Y','Y','Y','Y','N','N','Y','Y','Y');
INSERT INTO tmp_reverse_db VALUES ('%','test2\_%','','N','Y','Y','Y','N','Y','N','Y','Y','Y','Y','Y','Y','Y','Y','N','N','Y','Y','Y');
INSERT INTO reverse_db SELECT * FROM tmp_reverse_db WHERE @had_db_table=0;
DROP TABLE tmp_reverse_db;

-- Fill "db" table with default grants for anyone to
-- access database 'test', 'test1' and 'test2_%' if "db" table didn't exist
CREATE TEMPORARY TABLE tmp_reverse_db LIKE db;
INSERT INTO tmp_reverse_db VALUES ('%','test','','Y','Y','Y','Y','Y','Y','N','Y','Y','Y','Y','Y','Y','Y','Y','N','N','Y','Y','Y');
INSERT INTO tmp_reverse_db VALUES ('%','test1','','Y','Y','Y','Y','N','Y','N','Y','Y','Y','Y','Y','Y','Y','Y','N','N','Y','Y','Y');
INSERT INTO tmp_reverse_db VALUES ('%','test2\_%','','Y','Y','Y','Y','Y','Y','N','Y','Y','Y','Y','Y','Y','Y','Y','N','N','Y','Y','Y');
INSERT INTO reverse_db SELECT * FROM tmp_reverse_db WHERE @had_db_table=0;
DROP TABLE tmp_reverse_db;

-- Fill "user" table with default users allowing root access
-- from local machine if "user" table didn't exist before
CREATE TEMPORARY TABLE tmp_user_nopasswd LIKE user;
CREATE TEMPORARY TABLE tmp_user_socket LIKE user;
-- Classic passwordless root account.
INSERT INTO tmp_user_nopasswd VALUES ('localhost','root','','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','','','','',0,0,0,0,'','','N', 'N','', 0);
REPLACE INTO tmp_user_nopasswd SELECT @current_hostname,'root','','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','','','','',0,0,0,0,'','','N','N','',0 FROM dual WHERE @current_hostname != 'localhost';
REPLACE INTO tmp_user_nopasswd VALUES ('127.0.0.1','root','','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','','','','',0,0,0,0,'','','N','N','',0);
REPLACE INTO tmp_user_nopasswd VALUES ('::1','root','','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','','','','',0,0,0,0,'','','N','N', '', 0);
-- More secure root account using unix sucket auth.
INSERT INTO tmp_user_socket VALUES ('localhost',IFNULL(@auth_root_socket, 'root'),'','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','Y','','','','',0,0,0,0,'unix_socket','','N', 'N','', 0);

INSERT INTO user SELECT * FROM tmp_user_nopasswd WHERE @had_user_table=0 AND @skip_auth_root_nopasswd IS NULL;
INSERT INTO user SELECT * FROM tmp_user_socket WHERE @had_user_table=0 AND @auth_root_socket IS NOT NULL;
DROP TABLE tmp_user_nopasswd, tmp_user_socket;

CREATE TEMPORARY TABLE tmp_proxies_priv LIKE proxies_priv;
INSERT INTO tmp_proxies_priv VALUES ('localhost', 'root', '', '', TRUE, '', now());
REPLACE INTO tmp_proxies_priv SELECT @current_hostname, 'root', '', '', TRUE, '', now() FROM DUAL WHERE @current_hostname != 'localhost';
INSERT INTO  proxies_priv SELECT * FROM tmp_proxies_priv WHERE @had_proxies_priv_table=0;
DROP TABLE tmp_proxies_priv;

CREATE TEMPORARY TABLE tmp_reverse_proxies_priv LIKE reverse_proxies_priv;
INSERT INTO tmp_reverse_proxies_priv VALUES ('localhost', 'root', '', '', TRUE, '', now());
REPLACE INTO tmp_reverse_proxies_priv SELECT @current_hostname, 'root', '', '', TRUE, '', now() FROM DUAL WHERE @current_hostname != 'localhost';
INSERT INTO  proxies_priv SELECT * FROM tmp_reverse_proxies_priv WHERE @had_proxies_priv_table=0;
DROP TABLE tmp_reverse_proxies_priv;
