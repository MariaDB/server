-- Copyright (c) 2014, 2015, Oracle and/or its affiliates. All rights reserved.
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
-- Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA

--
-- Table: sys_config
--
-- Stores configuration options for sys objects
--

CREATE TABLE IF NOT EXISTS sys_config (
    variable VARCHAR(128) PRIMARY KEY,
    value VARCHAR(128),
    set_time TIMESTAMP(6) GENERATED ALWAYS AS ROW START,
    unset_time TIMESTAMP(6) GENERATED ALWAYS AS ROW END,
    set_by VARCHAR(128),
    PERIOD FOR SYSTEM_TIME(set_time, unset_time)
)
    ENGINE = Aria,
    WITH SYSTEM VERSIONING;


