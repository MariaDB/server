/*****************************************************************************

Copyright (c) 2022, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**
  @file include/sql_funcs.h

  SQL procedures for InnoDB system tables operation
*/

/**
  Rename foreign keys for rename table

  @see row_rename_table_for_mysql()
*/
constexpr const char *rename_constraint_ids=
R"===(PROCEDURE RENAME_CONSTRAINT_IDS () IS
  gen_constr_prefix CHAR;
  new_db_name CHAR;
  foreign_id CHAR;
  new_foreign_id CHAR;
  old_db_name_len INT;
  old_t_name_len INT;
  new_db_name_len INT;
  id_len INT;
  offset INT;
  found INT;
  BEGIN
  found := 1;
  old_db_name_len := INSTR(:old_table_name, '/') - 1;
  new_db_name_len := INSTR(:new_table_name, '/') - 1;
  new_db_name := SUBSTR(:new_table_name, 0,
                        new_db_name_len);
  old_t_name_len := LENGTH(:old_table_name);
  gen_constr_prefix := CONCAT(:old_table_name_utf8,
                              '_ibfk_');
  WHILE found = 1 LOOP
    SELECT ID INTO foreign_id
      FROM SYS_FOREIGN
      WHERE FOR_NAME = :old_table_name
      AND TO_BINARY(FOR_NAME)
        = TO_BINARY(:old_table_name)
      LOCK IN SHARE MODE;
    IF (SQL % NOTFOUND) THEN
      found := 0;
    ELSE
      UPDATE SYS_FOREIGN
      SET FOR_NAME = :new_table_name
      WHERE ID = foreign_id;
      id_len := LENGTH(foreign_id);
      IF (INSTR(foreign_id, '/') > 0) THEN
        IF (INSTR(foreign_id,
                  gen_constr_prefix) > 0)
        THEN
          offset := INSTR(foreign_id, '_ibfk_') - 1;
          new_foreign_id :=
          CONCAT(:new_table_utf8,
                 SUBSTR(foreign_id, offset, id_len - offset));
        ELSE
          new_foreign_id :=
          CONCAT(new_db_name,
                  SUBSTR(foreign_id, old_db_name_len,
                         id_len - old_db_name_len));
        END IF;
        UPDATE SYS_FOREIGN
          SET ID = new_foreign_id
          WHERE ID = foreign_id;
        UPDATE SYS_FOREIGN_COLS
          SET ID = new_foreign_id
          WHERE ID = foreign_id;
      END IF;
    END IF;
  END LOOP;
  UPDATE SYS_FOREIGN SET REF_NAME = :new_table_name
  WHERE REF_NAME = :old_table_name
    AND TO_BINARY(REF_NAME)
      = TO_BINARY(:old_table_name);
END;)===";
