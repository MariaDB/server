-- Copyright (C) 2014 MariaDB Ab.
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


-- This part creates stored procedures required by the OpenGIS standards.
-- Proc privilege is needed to run it.
-- To use this file, load its contents into the mysql database like that:
--     mysql -u root -p mysql < scripts/maria_add_gis_sp.sql

SET sql_mode='';

DROP PROCEDURE IF EXISTS AddGeometryColumn;
DROP PROCEDURE IF EXISTS DropGeometryColumn;

delimiter |

CREATE PROCEDURE AddGeometryColumn(catalog varchar(64), t_schema varchar(64),
   t_name varchar(64), geometry_column varchar(64), t_srid int)
begin
  set @qwe= concat('ALTER TABLE ', t_schema, '.', t_name, ' ADD ', geometry_column,' GEOMETRY REF_SYSTEM_ID=', t_srid);
  PREPARE ls from @qwe;
  execute ls;
  deallocate prepare ls;
end|

CREATE PROCEDURE DropGeometryColumn(catalog varchar(64), t_schema varchar(64),
   t_name varchar(64), geometry_column varchar(64))
begin
  set @qwe= concat('ALTER TABLE ', t_schema, '.', t_name, ' DROP ', geometry_column);
  PREPARE ls from @qwe;
  execute ls;
  deallocate prepare ls;
end|

delimiter ;

