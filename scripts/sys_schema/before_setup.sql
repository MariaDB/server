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

SET NAMES utf8 COLLATE utf8_general_ci;
SET @sql_log_bin = @@sql_log_bin;
SET sql_log_bin = 0;

CREATE DATABASE IF NOT EXISTS sys DEFAULT CHARACTER SET utf8mb3 COLLATE utf8mb3_general_ci;

-- If the database had existed, let's recreate its db.opt:
-- * to fix it if it contained unexpected charset/collation values
-- * to create it if it was removed in a mistake
ALTER DATABASE sys CHARACTER SET utf8mb3 COLLATE utf8mb3_general_ci;

USE sys;
