DROP PACKAGE IF EXISTS dbms_random;

SET @old_sql_mode = @@session.sql_mode, @@session.sql_mode = oracle;

DELIMITER $$

CREATE DEFINER='mariadb.sys'@'localhost' PACKAGE dbms_random
  SQL SECURITY INVOKER
  COMMENT 'Collection of random routines'
  AS
    PROCEDURE initialize (input INT)
      SQL SECURITY INVOKER
      COMMENT '
              Description
              -----------
              Initializes the seed

              Raises
              ------

              '
    ;
    PROCEDURE terminate
      SQL SECURITY INVOKER
      COMMENT '
              Description
              -----------
              Terminates package

              Raises
              ------

              '
    ;
    FUNCTION value RETURN DECIMAL(65,38)
      SQL SECURITY INVOKER
      COMMENT '
              Description
              -----------
              Gets a random number greater than or equal to 0 and less than 1,
              with 38 digits to the right of the decimal point (38-digit
              precision)

              Raises
              ------

              '
    ;
    FUNCTION random RETURN INT
      SQL SECURITY INVOKER
      COMMENT '
              Description
              -----------
              Generates a random number

              Raises
              ------

              '
    ;
END
$$

CREATE DEFINER='mariadb.sys'@'localhost' PACKAGE BODY dbms_random
  SQL SECURITY INVOKER
  AS
    FUNCTION get_seed_from_session() RETURN INTEGER
    AS
      v_user_hash INTEGER;
      v_time_num INTEGER;
      v_process_id INTEGER;
      v_seed INTEGER;
      v_username VARCHAR(128);
      v_i INTEGER;
    BEGIN
      -- Get current user name
      v_username := USER();
      -- Convert username to numeric hash (sum of ASCII values * position)
      v_user_hash := 0;
      FOR v_i IN 1..LENGTH(v_username) LOOP
        v_user_hash := v_user_hash + (ASCII(SUBSTR(v_username, v_i, 1)) * v_i);
      END LOOP;
      -- Get current time as Unix timestamp (seconds since epoch)
      v_time_num := UNIX_TIMESTAMP();
      -- Get process/connection ID
      v_process_id := CONNECTION_ID();
      -- Combine all three components into a single integer
      -- Using bit shifting and XOR to mix the values
      v_seed := MOD(
        (v_user_hash * 31) +
        (v_time_num * 17) +
        (v_process_id * 13),
        2147483647  -- Max 32-bit signed integer
      );
      RETURN v_seed;
    END;

    PROCEDURE initialize (input INT)
      SQL SECURITY INVOKER
      COMMENT '
          Description
          -----------
          Initializes the seed

          Raises
          ------

      '
    IS
    BEGIN
      SET @@rand_seed1 = input * 0x10001 + 55555555;
      SET @@rand_seed2 = input * 0x10000001;
    END;
    PROCEDURE terminate
      SQL SECURITY INVOKER
      COMMENT '
          Description
          -----------
          Terminates package

          Raises
          ------

      '
    IS
    BEGIN
    END;
    FUNCTION value RETURN DECIMAL(65,38)
      SQL SECURITY INVOKER
      COMMENT '
          Description
          -----------
          Gets a random number greater than or equal to 0 and less than 1, with
          38 digits to the right of the decimal point (38-digit precision)

          Raises
          ------

      '
    IS
    BEGIN
      RETURN CAST(RAND() AS DECIMAL(65, 38));
    END;
    FUNCTION random RETURN INT
      SQL SECURITY INVOKER
      COMMENT '
          Description
          -----------
          Generates a random number

          Raises
          ------

      '
    IS
    BEGIN
      RETURN CONVERT(RAND() * (POW(2, 31) - 1 + POW(2, 31)) - POW(2, 31), DECIMAL(65, 0));
    END;
    PROCEDURE set_seed
      SQL SECURITY INVOKER
      COMMENT '
          Description
          -----------
          Terminates package

          Raises
          ------

      '
    IS
    BEGIN
      SET @seed = get_seed_from_session();
      -- Set session-level random seed
      SET @@rand_seed1 = @seed * 0x10001 + 55555555;
      SET @@rand_seed2 = @seed * 0x10001;
    END;
    
BEGIN
  set_seed;
END
$$

DELIMITER ;

SET @@session.sql_mode = @old_sql_mode;
