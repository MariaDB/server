DROP PACKAGE IF EXISTS dbms_sql;

SET @old_sql_mode = @@session.sql_mode, @@session.sql_mode = oracle;

DELIMITER $$
CREATE DEFINER='mariadb.sys'@'localhost' PACKAGE dbms_sql
  SQL SECURITY INVOKER
  COMMENT 'Collection of sql routines'
  AS
    FUNCTION open_cursor RETURN INT
      SQL SECURITY INVOKER
      COMMENT '
              Description
              -----------
              This function opens a new cursor.

              Raises
              ------

              '
    ;
    PROCEDURE parse (cursor_id INT, input VARCHAR2(65511), language_flag INT)
      SQL SECURITY INVOKER
      COMMENT '
              Description
              -----------
              This procedure parses the given statement in the given cursor. All statements are parsed immediately. In addition, DDL statements are run immediately when parsed.

              Raises
              ------

              '
    ;
    FUNCTION execute (cursor_id INT) RETURN INT
      SQL SECURITY INVOKER
      COMMENT '
              Description
              -----------
              This function executes a given cursor. This function accepts the ID number of the cursor and returns the number of rows processed.

              Raises
              ------

              '
    ;
    PROCEDURE close_cursor (cursor_id INT)
      SQL SECURITY INVOKER
      COMMENT '
              Description
              -----------
              When you no longer need a cursor for a session, close the cursor by calling the CLOSE_CURSOR Procedure.

              Raises
              ------

              '
    ;
END
$$

CREATE DEFINER='mariadb.sys'@'localhost' PACKAGE BODY dbms_sql
  SQL SECURITY INVOKER
  AS
    FUNCTION open_cursor RETURN INT
      SQL SECURITY INVOKER
      COMMENT '
          Description
          -----------
          This function opens a new cursor.

          Raises
          ------

      '
    AS
    BEGIN
      DECLARE
        cursor_id INT;
      BEGIN
        SET cursor_id = dbms_sql_open_cursor();
        RETURN cursor_id;
      END;
    END;
    PROCEDURE parse (cursor_id INT, input VARCHAR2(65511), language_flag INT)
      SQL SECURITY INVOKER
      COMMENT '
          Description
          -----------
          This procedure parses the given statement in the given cursor. All statements are parsed immediately. In addition, DDL statements are run immediately when parsed.

          Raises
          ------

      '
    AS
    BEGIN
      DECLARE
        first_word VARCHAR2(64);
        second_word VARCHAR2(64);
        final_ps VARCHAR2(256);
        first_word_id INT;
        parse_result INT;
      BEGIN
        SET first_word = LOWER(SUBSTRING_INDEX(LTRIM(input), ' ', 1));
        SET second_word = SUBSTRING_INDEX(SUBSTRING_INDEX(input, ' ', 2), ' ', -1);
        IF first_word IN ('create', 'alter', 'drop', 'insert', 'update', 'delete') OR
            (first_word = 'rename' AND second_word IN ('table', 'tables')) THEN
          IF first_word IN ('create', 'alter', 'drop') OR
              (first_word = 'rename' AND second_word IN ('table', 'tables')) THEN
            SET final_ps = CONCAT('CREATE PROCEDURE final_NESTED_PROC() AS BEGIN PREPARE s', cursor_id, ' FROM \'', input, '\'; EXECUTE s', cursor_id, '; END');
            CASE first_word
              WHEN 'create' THEN
                SET first_word_id = 0;
              WHEN 'alter' THEN
                SET first_word_id = 1;
              WHEN 'drop' THEN
                SET first_word_id = 2;
              WHEN 'rename' THEN
                SET first_word_id = 3;
            END CASE;
          ELSIF first_word IN ('insert', 'update', 'delete') THEN
            SET final_ps = CONCAT('CREATE PROCEDURE final_NESTED_PROC() AS BEGIN PREPARE s', cursor_id, ' FROM \'', input, '\'; END');CASE first_word
              WHEN 'insert' THEN
                SET first_word_id = 4;
              WHEN 'update' THEN
                SET first_word_id = 5;
              WHEN 'delete' THEN
                SET first_word_id = 6;
            END CASE;
          END IF;
          EXECUTE IMMEDIATE final_ps;
          EXECUTE IMMEDIATE 'CALL final_NESTED_PROC()';
          EXECUTE IMMEDIATE 'DROP PROCEDURE final_NESTED_PROC';
          SET parse_result = dbms_sql_parse(cursor_id, first_word_id);
        ELSE
          SIGNAL SQLSTATE 'HY000' set mysql_errno=3047, message_text='missing or invalid option'; 
        END IF;
      END;

    END;
    FUNCTION execute (cursor_id INT) RETURN INT
      SQL SECURITY INVOKER
      COMMENT '
          Description
          -----------
          This function executes a given cursor. This function accepts the ID number of the cursor and returns the number of rows processed.

          Raises
          ------

      '
    AS
    BEGIN
      DECLARE
        element VARCHAR2(32);
      BEGIN
        SET element = CONCAT('s', cursor_id);
        CALL dbmssql_execute (element);
        RETURN ROW_COUNT();
      END;

    END;
    PROCEDURE close_cursor (cursor_id INT)
      SQL SECURITY INVOKER
      COMMENT '
          Description
          -----------
          When you no longer need a cursor for a session, close the cursor by calling the CLOSE_CURSOR Procedure.

          Raises
          ------

      '
    AS
    BEGIN
      DECLARE
        final_ps VARCHAR2(256);
        res INT;
      BEGIN
        set res = dbms_sql_close_cursor(cursor_id);
        SET final_ps = CONCAT('CREATE PROCEDURE NESTED_PROC() AS BEGIN DEALLOCATE PREPARE s', cursor_id, '; END');
        EXECUTE IMMEDIATE final_ps;
        EXECUTE IMMEDIATE 'CALL NESTED_PROC()';
        EXECUTE IMMEDIATE 'DROP PROCEDURE NESTED_PROC';
      END;

    END;

END
$$

DELIMITER ;

SET @@session.sql_mode = @old_sql_mode;
