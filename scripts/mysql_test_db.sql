-- Copyright (c) 2018 MariaDB Foundation
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

CREATE DATABASE IF NOT EXISTS test CHARACTER SET latin1 COLLATE latin1_swedish_ci;

-- Fill "db" table with default grants for anyone to
-- access database 'test' and 'test_%' if "db" table didn't exist
CREATE TEMPORARY TABLE tmp_db LIKE db;
INSERT INTO tmp_db VALUES ('%','test','','Y','Y','Y','Y','Y','Y','N','Y','Y','Y','Y','Y','Y','Y','Y','N','N','Y','Y','Y');
INSERT INTO tmp_db VALUES ('%','test\_%','','Y','Y','Y','Y','Y','Y','N','Y','Y','Y','Y','Y','Y','Y','Y','N','N','Y','Y','Y');
INSERT INTO db SELECT * FROM tmp_db WHERE @had_db_table=0;
DROP TABLE tmp_db;

-- Anonymous user with no privileges.
CREATE TEMPORARY TABLE tmp_user_anonymous LIKE global_priv;
INSERT INTO tmp_user_anonymous (host,user) VALUES ('localhost','');
INSERT INTO tmp_user_anonymous (host,user) SELECT @current_hostname,'' FROM dual WHERE @current_hostname != 'localhost';
INSERT INTO global_priv SELECT * FROM tmp_user_anonymous WHERE @had_user_table=0;
DROP TABLE tmp_user_anonymous;
