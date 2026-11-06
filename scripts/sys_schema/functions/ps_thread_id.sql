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

DELIMITER $$

CREATE OR REPLACE DEFINER='mariadb.sys'@'localhost' FUNCTION ps_thread_id (
        in_connection_id BIGINT UNSIGNED
    ) RETURNS BIGINT UNSIGNED
    COMMENT '
             Description
             -----------

             Return the Performance Schema THREAD_ID for the specified connection ID.

             Parameters
             -----------

             in_connection_id (BIGINT UNSIGNED):
               The id of the connection to return the thread id for. If NULL, the current
               connection thread id is returned.

             Example
             -----------

             mysql> SELECT sys.ps_thread_id(79);
             +----------------------+
             | sys.ps_thread_id(79) |
             +----------------------+
             |                   98 |
             +----------------------+
             1 row in set (0.00 sec)

             mysql> SELECT sys.ps_thread_id(CONNECTION_ID());
             +-----------------------------------+
             | sys.ps_thread_id(CONNECTION_ID()) |
             +-----------------------------------+
             |                                98 |
             +-----------------------------------+
             1 row in set (0.00 sec)
            '

    SQL SECURITY INVOKER
    NOT DETERMINISTIC
    READS SQL DATA
BEGIN
    -- The native functions raise an error when the Performance Schema
    -- is disabled; keep returning NULL, as this function always did.
    IF NOT @@performance_schema THEN
        RETURN NULL;
    END IF;
    RETURN IF(in_connection_id IS NULL,
              PS_CURRENT_THREAD_ID(),
              PS_THREAD_ID(in_connection_id));
END$$

DELIMITER ;
