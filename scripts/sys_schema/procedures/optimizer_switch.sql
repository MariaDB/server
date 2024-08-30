--  Copyright (C) 2023, MariaDB
--
--  This program is free software; you can redistribute it and/or modify
--  it under the terms of the GNU General Public License as published by
--  the Free Software Foundation; version 2 of the License.
--
--  This program is distributed in the hope that it will be useful,
--  but WITHOUT ANY WARRANTY; without even the implied warranty of
--  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
--  GNU General Public License for more details.
--
--  You should have received a copy of the GNU General Public License
--  along with this program; if not, write to the Free Software
--  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA

DROP PROCEDURE IF EXISTS optimizer_switch_choice;
DROP PROCEDURE IF EXISTS optimizer_switch_on;
DROP PROCEDURE IF EXISTS optimizer_switch_off;
DELIMITER $$

CREATE DEFINER='mariadb.sys'@'localhost' PROCEDURE optimizer_switch_choice(IN on_off VARCHAR(3))
COMMENT 'return @@optimizer_switch options as a result set for easier readability'
SQL SECURITY INVOKER
NOT DETERMINISTIC
CONTAINS SQL
BEGIN
  DECLARE tmp VARCHAR(1024);
  DECLARE opt VARCHAR(1024);
  DECLARE start INT;
  DECLARE end INT;
  DECLARE pos INT;
  set tmp=concat(@@optimizer_switch,",");
  CREATE OR REPLACE TEMPORARY TABLE tmp_opt_switch (a varchar(64), opt CHAR(3)) character set latin1 engine=heap;
  set start=1;
  FIND_OPTIONS:
  LOOP
    set pos= INSTR(SUBSTR(tmp, start), ",");
    if (pos = 0) THEN
       LEAVE FIND_OPTIONS;
    END IF;
    set opt= MID(tmp, start, pos-1);
    set end= INSTR(opt, "=");
    insert into tmp_opt_switch values(LEFT(opt,end-1),SUBSTR(opt,end+1));
    set start=start + pos;
  END LOOP;
  SELECT  t.a as "option",t.opt from tmp_opt_switch as t where t.opt = on_off order by a;
  DROP TEMPORARY TABLE tmp_opt_switch;
END$$

CREATE DEFINER='mariadb.sys'@'localhost' PROCEDURE optimizer_switch_on()
COMMENT 'return @@optimizer_switch options that are on'
SQL SECURITY INVOKER
NOT DETERMINISTIC
CONTAINS SQL
BEGIN
  call optimizer_switch_choice("on");
END$$

CREATE DEFINER='mariadb.sys'@'localhost' PROCEDURE optimizer_switch_off()
COMMENT 'return @@optimizer_switch options that are off'
SQL SECURITY INVOKER
NOT DETERMINISTIC
CONTAINS SQL
BEGIN
  call optimizer_switch_choice("off");
END$$

DELIMITER ;

