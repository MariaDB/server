-- Copyright (c) 2014, Monty Program Ab & SkySQL Ab
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
-- The system tables of MySQL Server
--

set sql_mode='';
DELETE FROM mysql.db WHERE Db='mariadb_spatial';
DROP DATABASE IF EXISTS mariadb_spatial;
CREATE DATABASE IF NOT EXISTS mariadb_spatial character set utf8;
USE mariadb_spatial;

CREATE TABLE IF NOT EXISTS reference_systems(id INT NOT NULL, alias_code INT NOT NULL, alias_name VARCHAR(80) binary DEFAULT '' NOT NULL, authority VARCHAR(256),  proj_def TEXT, PRIMARY KEY(id)) engine=MyISAM CHARACTER SET utf8 COLLATE utf8_bin comment='Reference systems';

CREATE TABLE IF NOT EXISTS columns (db char(64) binary DEFAULT '' NOT NULL, table_name char(64) binary DEFAULT '' NOT NULL, column_name char(64) binary DEFAULT '' NOT NULL, ref_system INT NOT NULL, wkb_geometry_type INT, dimension INT, UNIQUE (db, table_name, column_name))  engine=MyISAM CHARACTER SET utf8 COLLATE utf8_bin comment='Spatial columns';

INSERT INTO mysql.db SET Host='%', Db='mariadb_spatial', Select_priv='Y';
FLUSH PRIVILEGES;

INSERT INTO reference_systems VALUES (-1, 0, "Unbound planar coordinates", "", "");
INSERT INTO reference_systems VALUES ( 0, 0, "Unbound planar coordinates", "", "");
INSERT INTO reference_systems VALUES (4326,4326,'WGS 84', 'EPSG','GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563,AUTHORITY["EPSG","7030"]],AUTHORITY["EPSG","6326"]],PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]],AUTHORITY["EPSG","4326"]]');

