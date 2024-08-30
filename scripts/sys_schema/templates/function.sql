-- Copyright (c) 2015, Oracle and/or its affiliates. All rights reserved.
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

DROP FUNCTION IF EXISTS <function name>;

DELIMITER $$

CREATE DEFINER='mariadb.sys'@'localhost' FUNCTION <function name> (
        /* Variables */
    )
    RETURNS <data type>
    COMMENT '
             Description
             -----------

             ...

             Parameters
             -----------

             ...

             Returns
             -----------

             <data type>

             Example
             -----------

             ...

            '
    SQL SECURITY INVOKER
    { DETERMINISTIC | NOT DETERMINISTIC }
    { CONTAINS SQL | NO SQL | READS SQL DATA | MODIFIES SQL DATA }
BEGIN
  
    /* BODY */
    RETURN <something>;

END$$

DELIMITER ;
