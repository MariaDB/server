DROP PACKAGE IF EXISTS UTL_I18N;

SET @old_sql_mode = @@session.sql_mode, @@session.sql_mode = oracle;

DELIMITER $$

CREATE DEFINER='mariadb.sys'@'localhost' PACKAGE UTL_I18N
  SQL SECURITY INVOKER
  COMMENT 'Collection of routines to manipulate RAW data'
  AS
    FUNCTION transliterate(val VARCHAR2(255) CHARACTER SET ANY_CS, name VARCHAR2(255)) RETURN VARCHAR2(255) CHARACTER SET ANY_CS
      SQL SECURITY INVOKER
      COMMENT '
              Description
              -----------

              This function performs script transliteration.

              Parameters
              -----------

              val (VARCHAR2):
                Specifies the data to be converted.
              name (VARCHAR2):
                Specifies the transliteration name string.

              Returns
              -------

                The converted string.
              '
    ;
    FUNCTION raw_to_char(jc RAW, charset_or_collation VARCHAR(255)) RETURN VARCHAR2
      SQL SECURITY INVOKER
      COMMENT '
              Description
              -----------

              This function converts RAW data from a valid character set to a
              VARCHAR2 string in the database character set.

              Parameters
              -----------

              jc (RAW):
                Specifies the RAW data to be converted to a VARCHAR2 string
              charset_or_collation (VARCHAR):
                Specifies the character set that the RAW data was derived from.

              Returns
              -------

                the VARCHAR2 string equivalent in the database character set of
                the RAW data.
              '
    ;
    FUNCTION string_to_raw(jc VARCHAR2, charset_or_collation VARCHAR(255)) RETURN RAW
      SQL SECURITY INVOKER
      COMMENT '
              Description
              -----------

              This function converts a VARCHAR2 string to another valid
              character set and returns the result as RAW data.

              Parameters
              -----------

              jc (VARCHAR2):
                Specifies the VARCHAR2 or NVARCHAR2 string to convert.
              charset_or_collation (VARCHAR):
                Specifies the destination character set.

              Returns
              -------

                RAW data representation of the input string in the new character set
              '
    ;
END
$$

CREATE DEFINER='mariadb.sys'@'localhost' PACKAGE BODY UTL_I18N
  SQL SECURITY INVOKER
  AS
    FUNCTION transliterate(val VARCHAR2(255) CHARACTER SET ANY_CS, name VARCHAR2(255)) RETURN VARCHAR2(255) CHARACTER SET ANY_CS
      SQL SECURITY INVOKER
      COMMENT '
              Description
              -----------

              This function performs script transliteration.

              Parameters
              -----------

              val (VARCHAR2):
                Specifies the data to be converted.
              name (VARCHAR2):
                Specifies the transliteration name string.

              Returns
              -------

                The converted string.
              '
    AS
    BEGIN
      RETURN transliterate(val, name);
    END;

    FUNCTION raw_to_char(jc RAW, charset_or_collation VARCHAR(255)) RETURN VARCHAR2
      SQL SECURITY INVOKER
      COMMENT '
              Description
              -----------

              This function converts RAW data from a valid character set to a
              VARCHAR2 string in the database character set.

              Parameters
              -----------

              jc (RAW):
                Specifies the RAW data to be converted to a VARCHAR2 string
              charset_or_collation (VARCHAR):
                Specifies the character set that the RAW data was derived from.

              Returns
              -------

                the VARCHAR2 string equivalent in the database character set of
                the RAW data.
        '
    IS
    BEGIN
      DECLARE
        dst_charset VARCHAR(65532);
        sourced_jc VARCHAR(65532);
        targeted_sourced_jc VARCHAR(65532);
        unhexed_hexed_data BLOB;
      BEGIN
        SELECT VARIABLE_VALUE FROM INFORMATION_SCHEMA.SESSION_VARIABLES WHERE VARIABLE_NAME = 'character_set_results' into dst_charset;

        CASE charset_or_collation
          WHEN 'utf8' THEN
            CASE dst_charset
              WHEN 'utf8mb3' THEN
                BEGIN
                  SET sourced_jc = CONVERT(jc USING utf8);
                  SET targeted_sourced_jc = CONVERT(sourced_jc USING utf8mb3);
                END;
              ELSE
                RETURN NULL;
            END CASE;
          WHEN 'utf8mb3' THEN
            CASE dst_charset
              WHEN 'utf8mb4' THEN
                BEGIN
                  SET sourced_jc = CONVERT(jc USING utf8mb3);
                  SET targeted_sourced_jc = CONVERT(sourced_jc USING utf8mb4);
                END;
              ELSE
                RETURN NULL;
            END CASE;
          ELSE
            RETURN NULL;
        END CASE;

        SET unhexed_hexed_data = UNHEX(HEX(targeted_sourced_jc));

        CASE dst_charset
          WHEN 'utf8mb3' THEN
            RETURN CONVERT(unhexed_hexed_data USING utf8mb3);
          WHEN 'utf8mb4' THEN
            RETURN CONVERT(unhexed_hexed_data USING utf8mb4);
        END CASE;
        RETURN NULL;
      END;

    END;

    FUNCTION string_to_raw(jc VARCHAR2, charset_or_collation VARCHAR(255)) RETURN RAW
      SQL SECURITY INVOKER
      COMMENT '
              Description
              -----------

              This function converts a VARCHAR2 string to another valid
              character set and returns the result as RAW data.

              Parameters
              -----------

              jc (VARCHAR2):
                Specifies the VARCHAR2 or NVARCHAR2 string to convert.
              charset_or_collation (VARCHAR):
                Specifies the destination character set.

              Returns
              -------

                RAW data representation of the input string in the new character set
              '
    AS
    BEGIN
      CASE charset_or_collation
        WHEN 'utf8' THEN
          RETURN CAST(CONVERT(jc USING utf8mb4) AS BINARY);
        WHEN 'ucs2' THEN
          RETURN CAST(CONVERT(jc USING ucs2) AS BINARY);
        ELSE
          RETURN NULL;
      END CASE;
    END;


END
$$

DELIMITER ;

SET @@session.sql_mode = @old_sql_mode;
