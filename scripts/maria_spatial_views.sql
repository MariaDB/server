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

DROP VIEW IF EXISTS SPATIAL_REF_SYS;
DROP VIEW IF EXISTS GEOMETRY_COLUMNS;

CREATE VIEW SPATIAL_REF_SYS AS SELECT id as SRID, authority as AUTH_NAME, alias_code as AUTH_SRID, proj_def as SRTEXT FROM mariadb_spatial.reference_systems;

CREATE VIEW GEOMETRY_COLUMNS AS SELECT '' as F_TABLE_CATALOG, db as F_TABLE_SCHEMA, table_name as F_TABLE_NAME, column_name as F_GEOMETRY_COLUMN, '' as G_TABLE_CATALOG, db as G_TABLE_SCHEMA, table_name as G_TABLE_NAME, column_name as G_GEOMETRY_COLUMN, 1 as STORAGE_TYPE, wkb_geometry_type as GEOMETRY_TYPE, dimension as COORD_DIMENSION, NULL as MAX_PPR, ref_system as SRID from mariadb_spatial.columns;

