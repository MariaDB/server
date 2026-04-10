/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison interface for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2021 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

#ifndef YY_MYSQL_USERS_ARYANKANCHERLA_DOWNLOADS_DLAB_MARIADB_SERVER_MARIADB_BUILD_SQL_YY_MARIADB_HH_INCLUDED
# define YY_MYSQL_USERS_ARYANKANCHERLA_DOWNLOADS_DLAB_MARIADB_SERVER_MARIADB_BUILD_SQL_YY_MARIADB_HH_INCLUDED
/* Debug traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif
#if YYDEBUG
extern int MYSQLdebug;
#endif
/* "%code requires" blocks.  */
#line 197 "/Users/aryankancherla/Downloads/DLAB_MariaDB/server/sql/sql_yacc.yy"

// Master_info_file, enum_master_use_gtid, std::optional
#include "rpl_master_info_file.h"

#line 54 "/Users/aryankancherla/Downloads/DLAB_MariaDB/server/mariadb-build/sql/yy_mariadb.hh"

/* Token kinds.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
  enum yytokentype
  {
    YYEMPTY = -2,
    YYEOF = 0,                     /* "end of file"  */
    YYerror = 256,                 /* error  */
    YYUNDEF = 257,                 /* "invalid token"  */
    HINT_COMMENT = 258,            /* HINT_COMMENT  */
    ABORT_SYM = 259,               /* ABORT_SYM  */
    IMPOSSIBLE_ACTION = 260,       /* IMPOSSIBLE_ACTION  */
    FORCE_LOOKAHEAD = 261,         /* FORCE_LOOKAHEAD  */
    END_OF_INPUT = 262,            /* END_OF_INPUT  */
    COLON_ORACLE_SYM = 263,        /* COLON_ORACLE_SYM  */
    PARAM_MARKER = 264,            /* PARAM_MARKER  */
    FOR_SYSTEM_TIME_SYM = 265,     /* FOR_SYSTEM_TIME_SYM  */
    LEFT_PAREN_ALT = 266,          /* LEFT_PAREN_ALT  */
    LEFT_PAREN_WITH = 267,         /* LEFT_PAREN_WITH  */
    LEFT_PAREN_LIKE = 268,         /* LEFT_PAREN_LIKE  */
    ORACLE_CONCAT_SYM = 269,       /* ORACLE_CONCAT_SYM  */
    PERCENT_ORACLE_SYM = 270,      /* PERCENT_ORACLE_SYM  */
    WITH_CUBE_SYM = 271,           /* WITH_CUBE_SYM  */
    WITH_ROLLUP_SYM = 272,         /* WITH_ROLLUP_SYM  */
    WITH_SYSTEM_SYM = 273,         /* WITH_SYSTEM_SYM  */
    IDENT = 274,                   /* IDENT  */
    IDENT_QUOTED = 275,            /* IDENT_QUOTED  */
    LEX_HOSTNAME = 276,            /* LEX_HOSTNAME  */
    UNDERSCORE_CHARSET = 277,      /* UNDERSCORE_CHARSET  */
    BIN_NUM = 278,                 /* BIN_NUM  */
    DECIMAL_NUM = 279,             /* DECIMAL_NUM  */
    FLOAT_NUM = 280,               /* FLOAT_NUM  */
    HEX_NUM = 281,                 /* HEX_NUM  */
    HEX_STRING = 282,              /* HEX_STRING  */
    LONG_NUM = 283,                /* LONG_NUM  */
    NCHAR_STRING = 284,            /* NCHAR_STRING  */
    NUM = 285,                     /* NUM  */
    TEXT_STRING = 286,             /* TEXT_STRING  */
    ULONGLONG_NUM = 287,           /* ULONGLONG_NUM  */
    AND_AND_SYM = 288,             /* AND_AND_SYM  */
    DOT_DOT_SYM = 289,             /* DOT_DOT_SYM  */
    EQUAL_SYM = 290,               /* EQUAL_SYM  */
    GE = 291,                      /* GE  */
    LE = 292,                      /* LE  */
    MYSQL_CONCAT_SYM = 293,        /* MYSQL_CONCAT_SYM  */
    NE = 294,                      /* NE  */
    NOT2_SYM = 295,                /* NOT2_SYM  */
    OR2_SYM = 296,                 /* OR2_SYM  */
    SET_VAR = 297,                 /* SET_VAR  */
    SHIFT_LEFT = 298,              /* SHIFT_LEFT  */
    SHIFT_RIGHT = 299,             /* SHIFT_RIGHT  */
    ARROW_SYM = 300,               /* ARROW_SYM  */
    ADD = 301,                     /* ADD  */
    ALL = 302,                     /* ALL  */
    ALTER = 303,                   /* ALTER  */
    ANALYZE_SYM = 304,             /* ANALYZE_SYM  */
    AND_SYM = 305,                 /* AND_SYM  */
    ASC = 306,                     /* ASC  */
    ASENSITIVE_SYM = 307,          /* ASENSITIVE_SYM  */
    AS = 308,                      /* AS  */
    BEFORE_SYM = 309,              /* BEFORE_SYM  */
    BETWEEN_SYM = 310,             /* BETWEEN_SYM  */
    BIGINT = 311,                  /* BIGINT  */
    BINARY = 312,                  /* BINARY  */
    BIT_AND = 313,                 /* BIT_AND  */
    BIT_OR = 314,                  /* BIT_OR  */
    BIT_XOR = 315,                 /* BIT_XOR  */
    BLOB_MARIADB_SYM = 316,        /* BLOB_MARIADB_SYM  */
    BLOB_ORACLE_SYM = 317,         /* BLOB_ORACLE_SYM  */
    BODY_ORACLE_SYM = 318,         /* BODY_ORACLE_SYM  */
    BOTH = 319,                    /* BOTH  */
    BY = 320,                      /* BY  */
    CALL_SYM = 321,                /* CALL_SYM  */
    CASCADE = 322,                 /* CASCADE  */
    CASE_SYM = 323,                /* CASE_SYM  */
    CAST_SYM = 324,                /* CAST_SYM  */
    CHANGE = 325,                  /* CHANGE  */
    CHAR_SYM = 326,                /* CHAR_SYM  */
    CHECK_SYM = 327,               /* CHECK_SYM  */
    COLLATE_SYM = 328,             /* COLLATE_SYM  */
    CONDITION_SYM = 329,           /* CONDITION_SYM  */
    CONSTRAINT = 330,              /* CONSTRAINT  */
    CONTINUE_MARIADB_SYM = 331,    /* CONTINUE_MARIADB_SYM  */
    CONTINUE_ORACLE_SYM = 332,     /* CONTINUE_ORACLE_SYM  */
    CONVERT_SYM = 333,             /* CONVERT_SYM  */
    COUNT_SYM = 334,               /* COUNT_SYM  */
    CREATE = 335,                  /* CREATE  */
    CROSS = 336,                   /* CROSS  */
    CUME_DIST_SYM = 337,           /* CUME_DIST_SYM  */
    CURDATE = 338,                 /* CURDATE  */
    CURRENT_PATH = 339,            /* CURRENT_PATH  */
    CURRENT_ROLE = 340,            /* CURRENT_ROLE  */
    CURRENT_USER = 341,            /* CURRENT_USER  */
    CURSOR_SYM = 342,              /* CURSOR_SYM  */
    CURTIME = 343,                 /* CURTIME  */
    DATABASE = 344,                /* DATABASE  */
    DATABASES = 345,               /* DATABASES  */
    DATE_ADD_INTERVAL = 346,       /* DATE_ADD_INTERVAL  */
    DATE_SUB_INTERVAL = 347,       /* DATE_SUB_INTERVAL  */
    DAY_HOUR_SYM = 348,            /* DAY_HOUR_SYM  */
    DAY_MICROSECOND_SYM = 349,     /* DAY_MICROSECOND_SYM  */
    DAY_MINUTE_SYM = 350,          /* DAY_MINUTE_SYM  */
    DAY_SECOND_SYM = 351,          /* DAY_SECOND_SYM  */
    DECIMAL_SYM = 352,             /* DECIMAL_SYM  */
    DECLARE_MARIADB_SYM = 353,     /* DECLARE_MARIADB_SYM  */
    DECLARE_ORACLE_SYM = 354,      /* DECLARE_ORACLE_SYM  */
    DEFAULT = 355,                 /* DEFAULT  */
    DELETE_DOMAIN_ID_SYM = 356,    /* DELETE_DOMAIN_ID_SYM  */
    DELETE_SYM = 357,              /* DELETE_SYM  */
    DENSE_RANK_SYM = 358,          /* DENSE_RANK_SYM  */
    DESCRIBE = 359,                /* DESCRIBE  */
    DESC = 360,                    /* DESC  */
    DETERMINISTIC_SYM = 361,       /* DETERMINISTIC_SYM  */
    DISTINCT = 362,                /* DISTINCT  */
    DIV_SYM = 363,                 /* DIV_SYM  */
    DO_DOMAIN_IDS_SYM = 364,       /* DO_DOMAIN_IDS_SYM  */
    DOUBLE_SYM = 365,              /* DOUBLE_SYM  */
    DROP = 366,                    /* DROP  */
    DUAL_SYM = 367,                /* DUAL_SYM  */
    EACH_SYM = 368,                /* EACH_SYM  */
    ELSEIF_MARIADB_SYM = 369,      /* ELSEIF_MARIADB_SYM  */
    ELSE = 370,                    /* ELSE  */
    ELSIF_ORACLE_SYM = 371,        /* ELSIF_ORACLE_SYM  */
    EMPTY_SYM = 372,               /* EMPTY_SYM  */
    ENCLOSED = 373,                /* ENCLOSED  */
    ESCAPED = 374,                 /* ESCAPED  */
    EXCEPT_SYM = 375,              /* EXCEPT_SYM  */
    EXISTS = 376,                  /* EXISTS  */
    EXTRACT_SYM = 377,             /* EXTRACT_SYM  */
    FALSE_SYM = 378,               /* FALSE_SYM  */
    FETCH_SYM = 379,               /* FETCH_SYM  */
    FIRST_VALUE_SYM = 380,         /* FIRST_VALUE_SYM  */
    FLOAT_SYM = 381,               /* FLOAT_SYM  */
    FOREIGN = 382,                 /* FOREIGN  */
    FOR_SYM = 383,                 /* FOR_SYM  */
    FROM = 384,                    /* FROM  */
    FULLTEXT_SYM = 385,            /* FULLTEXT_SYM  */
    GOTO_ORACLE_SYM = 386,         /* GOTO_ORACLE_SYM  */
    GRANT = 387,                   /* GRANT  */
    GROUP_CONCAT_SYM = 388,        /* GROUP_CONCAT_SYM  */
    JSON_ARRAYAGG_SYM = 389,       /* JSON_ARRAYAGG_SYM  */
    JSON_OBJECTAGG_SYM = 390,      /* JSON_OBJECTAGG_SYM  */
    JSON_TABLE_SYM = 391,          /* JSON_TABLE_SYM  */
    GROUP_SYM = 392,               /* GROUP_SYM  */
    HAVING = 393,                  /* HAVING  */
    HOUR_MICROSECOND_SYM = 394,    /* HOUR_MICROSECOND_SYM  */
    HOUR_MINUTE_SYM = 395,         /* HOUR_MINUTE_SYM  */
    HOUR_SECOND_SYM = 396,         /* HOUR_SECOND_SYM  */
    IF_SYM = 397,                  /* IF_SYM  */
    IGNORE_DOMAIN_IDS_SYM = 398,   /* IGNORE_DOMAIN_IDS_SYM  */
    IGNORE_SYM = 399,              /* IGNORE_SYM  */
    IGNORED_SYM = 400,             /* IGNORED_SYM  */
    INDEX_SYM = 401,               /* INDEX_SYM  */
    INFILE = 402,                  /* INFILE  */
    INNER_SYM = 403,               /* INNER_SYM  */
    INOUT_SYM = 404,               /* INOUT_SYM  */
    INSENSITIVE_SYM = 405,         /* INSENSITIVE_SYM  */
    INSERT = 406,                  /* INSERT  */
    IN_SYM = 407,                  /* IN_SYM  */
    INTERSECT_SYM = 408,           /* INTERSECT_SYM  */
    INTERVAL_SYM = 409,            /* INTERVAL_SYM  */
    INTO = 410,                    /* INTO  */
    INT_SYM = 411,                 /* INT_SYM  */
    IS = 412,                      /* IS  */
    ITERATE_SYM = 413,             /* ITERATE_SYM  */
    JOIN_SYM = 414,                /* JOIN_SYM  */
    KEYS = 415,                    /* KEYS  */
    KEY_SYM = 416,                 /* KEY_SYM  */
    KILL_SYM = 417,                /* KILL_SYM  */
    LAG_SYM = 418,                 /* LAG_SYM  */
    LEADING = 419,                 /* LEADING  */
    LEAD_SYM = 420,                /* LEAD_SYM  */
    LEAVE_SYM = 421,               /* LEAVE_SYM  */
    LEFT = 422,                    /* LEFT  */
    LIKE = 423,                    /* LIKE  */
    LIMIT = 424,                   /* LIMIT  */
    LINEAR_SYM = 425,              /* LINEAR_SYM  */
    LINES = 426,                   /* LINES  */
    LOAD = 427,                    /* LOAD  */
    LOCALTIMESTAMP = 428,          /* LOCALTIMESTAMP  */
    LOCATOR_SYM = 429,             /* LOCATOR_SYM  */
    LOCK_SYM = 430,                /* LOCK_SYM  */
    LONGBLOB = 431,                /* LONGBLOB  */
    LONG_SYM = 432,                /* LONG_SYM  */
    LONGTEXT = 433,                /* LONGTEXT  */
    LOOP_SYM = 434,                /* LOOP_SYM  */
    LOW_PRIORITY = 435,            /* LOW_PRIORITY  */
    MASTER_SSL_VERIFY_SERVER_CERT_SYM = 436, /* MASTER_SSL_VERIFY_SERVER_CERT_SYM  */
    MATCH = 437,                   /* MATCH  */
    MAX_SYM = 438,                 /* MAX_SYM  */
    MAXVALUE_SYM = 439,            /* MAXVALUE_SYM  */
    MEDIAN_SYM = 440,              /* MEDIAN_SYM  */
    MEDIUMBLOB = 441,              /* MEDIUMBLOB  */
    MEDIUMINT = 442,               /* MEDIUMINT  */
    MEDIUMTEXT = 443,              /* MEDIUMTEXT  */
    MIN_SYM = 444,                 /* MIN_SYM  */
    MINUS_ORACLE_SYM = 445,        /* MINUS_ORACLE_SYM  */
    MINUTE_MICROSECOND_SYM = 446,  /* MINUTE_MICROSECOND_SYM  */
    MINUTE_SECOND_SYM = 447,       /* MINUTE_SECOND_SYM  */
    MODIFIES_SYM = 448,            /* MODIFIES_SYM  */
    MOD_SYM = 449,                 /* MOD_SYM  */
    NATURAL = 450,                 /* NATURAL  */
    NEG = 451,                     /* NEG  */
    NESTED_SYM = 452,              /* NESTED_SYM  */
    NOT_SYM = 453,                 /* NOT_SYM  */
    NO_WRITE_TO_BINLOG = 454,      /* NO_WRITE_TO_BINLOG  */
    NOW_SYM = 455,                 /* NOW_SYM  */
    NTH_VALUE_SYM = 456,           /* NTH_VALUE_SYM  */
    NTILE_SYM = 457,               /* NTILE_SYM  */
    NULL_SYM = 458,                /* NULL_SYM  */
    NUMERIC_SYM = 459,             /* NUMERIC_SYM  */
    ON = 460,                      /* ON  */
    OPTIMIZE = 461,                /* OPTIMIZE  */
    OPTIONALLY = 462,              /* OPTIONALLY  */
    ORDER_SYM = 463,               /* ORDER_SYM  */
    ORDINALITY_SYM = 464,          /* ORDINALITY_SYM  */
    OR_SYM = 465,                  /* OR_SYM  */
    OTHERS_ORACLE_SYM = 466,       /* OTHERS_ORACLE_SYM  */
    OUTER = 467,                   /* OUTER  */
    OUTFILE = 468,                 /* OUTFILE  */
    OUT_SYM = 469,                 /* OUT_SYM  */
    OVER_SYM = 470,                /* OVER_SYM  */
    PACKAGE_ORACLE_SYM = 471,      /* PACKAGE_ORACLE_SYM  */
    PAGE_CHECKSUM_SYM = 472,       /* PAGE_CHECKSUM_SYM  */
    PARSE_VCOL_EXPR_SYM = 473,     /* PARSE_VCOL_EXPR_SYM  */
    PARTITION_SYM = 474,           /* PARTITION_SYM  */
    PATH_SYM = 475,                /* PATH_SYM  */
    PERCENTILE_CONT_SYM = 476,     /* PERCENTILE_CONT_SYM  */
    PERCENTILE_DISC_SYM = 477,     /* PERCENTILE_DISC_SYM  */
    PERCENT_RANK_SYM = 478,        /* PERCENT_RANK_SYM  */
    PORTION_SYM = 479,             /* PORTION_SYM  */
    POSITION_SYM = 480,            /* POSITION_SYM  */
    PRECISION = 481,               /* PRECISION  */
    PRIMARY_SYM = 482,             /* PRIMARY_SYM  */
    PROCEDURE_SYM = 483,           /* PROCEDURE_SYM  */
    PURGE = 484,                   /* PURGE  */
    RAISE_ORACLE_SYM = 485,        /* RAISE_ORACLE_SYM  */
    RANGE_SYM = 486,               /* RANGE_SYM  */
    RANK_SYM = 487,                /* RANK_SYM  */
    READS_SYM = 488,               /* READS_SYM  */
    READ_SYM = 489,                /* READ_SYM  */
    READ_WRITE_SYM = 490,          /* READ_WRITE_SYM  */
    REAL = 491,                    /* REAL  */
    RECURSIVE_SYM = 492,           /* RECURSIVE_SYM  */
    REFERENCES = 493,              /* REFERENCES  */
    REF_SYSTEM_ID_SYM = 494,       /* REF_SYSTEM_ID_SYM  */
    REGEXP = 495,                  /* REGEXP  */
    RELEASE_SYM = 496,             /* RELEASE_SYM  */
    RENAME = 497,                  /* RENAME  */
    REPEAT_SYM = 498,              /* REPEAT_SYM  */
    REQUIRE_SYM = 499,             /* REQUIRE_SYM  */
    RESIGNAL_SYM = 500,            /* RESIGNAL_SYM  */
    RESTRICT = 501,                /* RESTRICT  */
    RETURNING_SYM = 502,           /* RETURNING_SYM  */
    RETURN_MARIADB_SYM = 503,      /* RETURN_MARIADB_SYM  */
    RETURN_ORACLE_SYM = 504,       /* RETURN_ORACLE_SYM  */
    REVOKE = 505,                  /* REVOKE  */
    RIGHT = 506,                   /* RIGHT  */
    ROW_NUMBER_SYM = 507,          /* ROW_NUMBER_SYM  */
    ROWS_SYM = 508,                /* ROWS_SYM  */
    ROWTYPE_ORACLE_SYM = 509,      /* ROWTYPE_ORACLE_SYM  */
    SECOND_MICROSECOND_SYM = 510,  /* SECOND_MICROSECOND_SYM  */
    SELECT_SYM = 511,              /* SELECT_SYM  */
    SENSITIVE_SYM = 512,           /* SENSITIVE_SYM  */
    SEPARATOR_SYM = 513,           /* SEPARATOR_SYM  */
    SET = 514,                     /* SET  */
    SHOW = 515,                    /* SHOW  */
    SIGNAL_SYM = 516,              /* SIGNAL_SYM  */
    SMALLINT = 517,                /* SMALLINT  */
    SPATIAL_SYM = 518,             /* SPATIAL_SYM  */
    SPECIFIC_SYM = 519,            /* SPECIFIC_SYM  */
    SQL_BIG_RESULT = 520,          /* SQL_BIG_RESULT  */
    SQLEXCEPTION_SYM = 521,        /* SQLEXCEPTION_SYM  */
    SQL_SMALL_RESULT = 522,        /* SQL_SMALL_RESULT  */
    SQLSTATE_SYM = 523,            /* SQLSTATE_SYM  */
    SQL_SYM = 524,                 /* SQL_SYM  */
    SQLWARNING_SYM = 525,          /* SQLWARNING_SYM  */
    SSL_SYM = 526,                 /* SSL_SYM  */
    STARTING = 527,                /* STARTING  */
    STATS_AUTO_RECALC_SYM = 528,   /* STATS_AUTO_RECALC_SYM  */
    STATS_PERSISTENT_SYM = 529,    /* STATS_PERSISTENT_SYM  */
    STATS_SAMPLE_PAGES_SYM = 530,  /* STATS_SAMPLE_PAGES_SYM  */
    STDDEV_SAMP_SYM = 531,         /* STDDEV_SAMP_SYM  */
    STD_SYM = 532,                 /* STD_SYM  */
    STRAIGHT_JOIN = 533,           /* STRAIGHT_JOIN  */
    SUM_SYM = 534,                 /* SUM_SYM  */
    SYSDATE = 535,                 /* SYSDATE  */
    TABLE_SYM = 536,               /* TABLE_SYM  */
    TERMINATED = 537,              /* TERMINATED  */
    THEN_SYM = 538,                /* THEN_SYM  */
    TINYBLOB = 539,                /* TINYBLOB  */
    TINYINT = 540,                 /* TINYINT  */
    TINYTEXT = 541,                /* TINYTEXT  */
    TO_SYM = 542,                  /* TO_SYM  */
    TRAILING = 543,                /* TRAILING  */
    TRIGGER_SYM = 544,             /* TRIGGER_SYM  */
    TRUE_SYM = 545,                /* TRUE_SYM  */
    UNDO_SYM = 546,                /* UNDO_SYM  */
    UNION_SYM = 547,               /* UNION_SYM  */
    UNIQUE_SYM = 548,              /* UNIQUE_SYM  */
    UNLOCK_SYM = 549,              /* UNLOCK_SYM  */
    UNSIGNED = 550,                /* UNSIGNED  */
    UPDATE_SYM = 551,              /* UPDATE_SYM  */
    USAGE = 552,                   /* USAGE  */
    USE_SYM = 553,                 /* USE_SYM  */
    USING = 554,                   /* USING  */
    UTC_DATE_SYM = 555,            /* UTC_DATE_SYM  */
    UTC_TIMESTAMP_SYM = 556,       /* UTC_TIMESTAMP_SYM  */
    UTC_TIME_SYM = 557,            /* UTC_TIME_SYM  */
    VALUES_IN_SYM = 558,           /* VALUES_IN_SYM  */
    VALUES_LESS_SYM = 559,         /* VALUES_LESS_SYM  */
    VALUES = 560,                  /* VALUES  */
    VARBINARY = 561,               /* VARBINARY  */
    VARCHAR = 562,                 /* VARCHAR  */
    VARIANCE_SYM = 563,            /* VARIANCE_SYM  */
    VAR_SAMP_SYM = 564,            /* VAR_SAMP_SYM  */
    VARYING = 565,                 /* VARYING  */
    VECTOR_SYM = 566,              /* VECTOR_SYM  */
    WHEN_SYM = 567,                /* WHEN_SYM  */
    WHERE = 568,                   /* WHERE  */
    WHILE_SYM = 569,               /* WHILE_SYM  */
    WITH = 570,                    /* WITH  */
    XOR = 571,                     /* XOR  */
    YEAR_MONTH_SYM = 572,          /* YEAR_MONTH_SYM  */
    ZEROFILL = 573,                /* ZEROFILL  */
    BODY_MARIADB_SYM = 574,        /* BODY_MARIADB_SYM  */
    ELSEIF_ORACLE_SYM = 575,       /* ELSEIF_ORACLE_SYM  */
    ELSIF_MARIADB_SYM = 576,       /* ELSIF_MARIADB_SYM  */
    EXCEPTION_ORACLE_SYM = 577,    /* EXCEPTION_ORACLE_SYM  */
    GOTO_MARIADB_SYM = 578,        /* GOTO_MARIADB_SYM  */
    NOCOPY_SYM = 579,              /* NOCOPY_SYM  */
    OTHERS_MARIADB_SYM = 580,      /* OTHERS_MARIADB_SYM  */
    PACKAGE_MARIADB_SYM = 581,     /* PACKAGE_MARIADB_SYM  */
    RAISE_MARIADB_SYM = 582,       /* RAISE_MARIADB_SYM  */
    RECORD_SYM = 583,              /* RECORD_SYM  */
    ROWTYPE_MARIADB_SYM = 584,     /* ROWTYPE_MARIADB_SYM  */
    ROWNUM_SYM = 585,              /* ROWNUM_SYM  */
    REPLACE = 586,                 /* REPLACE  */
    SUBSTRING = 587,               /* SUBSTRING  */
    TRIM = 588,                    /* TRIM  */
    ACCOUNT_SYM = 589,             /* ACCOUNT_SYM  */
    ACTION = 590,                  /* ACTION  */
    ADMIN_SYM = 591,               /* ADMIN_SYM  */
    ADDDATE_SYM = 592,             /* ADDDATE_SYM  */
    AFTER_SYM = 593,               /* AFTER_SYM  */
    AGAINST = 594,                 /* AGAINST  */
    AGGREGATE_SYM = 595,           /* AGGREGATE_SYM  */
    ALGORITHM_SYM = 596,           /* ALGORITHM_SYM  */
    ALWAYS_SYM = 597,              /* ALWAYS_SYM  */
    ANY_SYM = 598,                 /* ANY_SYM  */
    ARRAY_SYM = 599,               /* ARRAY_SYM  */
    ASCII_SYM = 600,               /* ASCII_SYM  */
    AT_SYM = 601,                  /* AT_SYM  */
    ATOMIC_SYM = 602,              /* ATOMIC_SYM  */
    AUTHORS_SYM = 603,             /* AUTHORS_SYM  */
    AUTHORIZATION_SYM = 604,       /* AUTHORIZATION_SYM  */
    AUTO_INC = 605,                /* AUTO_INC  */
    AUTO_SYM = 606,                /* AUTO_SYM  */
    AVG_ROW_LENGTH = 607,          /* AVG_ROW_LENGTH  */
    AVG_SYM = 608,                 /* AVG_SYM  */
    BACKUP_SYM = 609,              /* BACKUP_SYM  */
    BEGIN_MARIADB_SYM = 610,       /* BEGIN_MARIADB_SYM  */
    BEGIN_ORACLE_SYM = 611,        /* BEGIN_ORACLE_SYM  */
    BINLOG_SYM = 612,              /* BINLOG_SYM  */
    BIT_SYM = 613,                 /* BIT_SYM  */
    BLOCK_SYM = 614,               /* BLOCK_SYM  */
    BOOL_SYM = 615,                /* BOOL_SYM  */
    BOOLEAN_SYM = 616,             /* BOOLEAN_SYM  */
    BTREE_SYM = 617,               /* BTREE_SYM  */
    BYTE_SYM = 618,                /* BYTE_SYM  */
    CACHE_SYM = 619,               /* CACHE_SYM  */
    CASCADED = 620,                /* CASCADED  */
    CATALOG_NAME_SYM = 621,        /* CATALOG_NAME_SYM  */
    CHAIN_SYM = 622,               /* CHAIN_SYM  */
    CHANGED = 623,                 /* CHANGED  */
    CHANNEL_SYM = 624,             /* CHANNEL_SYM  */
    CHARSET = 625,                 /* CHARSET  */
    CHECKPOINT_SYM = 626,          /* CHECKPOINT_SYM  */
    CHECKSUM_SYM = 627,            /* CHECKSUM_SYM  */
    CIPHER_SYM = 628,              /* CIPHER_SYM  */
    CLASS_ORIGIN_SYM = 629,        /* CLASS_ORIGIN_SYM  */
    CLIENT_SYM = 630,              /* CLIENT_SYM  */
    CLOB_MARIADB_SYM = 631,        /* CLOB_MARIADB_SYM  */
    CLOB_ORACLE_SYM = 632,         /* CLOB_ORACLE_SYM  */
    CLOSE_SYM = 633,               /* CLOSE_SYM  */
    COALESCE = 634,                /* COALESCE  */
    CODE_SYM = 635,                /* CODE_SYM  */
    COLLATION_SYM = 636,           /* COLLATION_SYM  */
    COLUMNS = 637,                 /* COLUMNS  */
    COLUMN_ADD_SYM = 638,          /* COLUMN_ADD_SYM  */
    COLUMN_CHECK_SYM = 639,        /* COLUMN_CHECK_SYM  */
    COLUMN_CREATE_SYM = 640,       /* COLUMN_CREATE_SYM  */
    COLUMN_DELETE_SYM = 641,       /* COLUMN_DELETE_SYM  */
    COLUMN_GET_SYM = 642,          /* COLUMN_GET_SYM  */
    COLUMN_SYM = 643,              /* COLUMN_SYM  */
    COLUMN_NAME_SYM = 644,         /* COLUMN_NAME_SYM  */
    COMMENT_SYM = 645,             /* COMMENT_SYM  */
    COMMITTED_SYM = 646,           /* COMMITTED_SYM  */
    COMMIT_SYM = 647,              /* COMMIT_SYM  */
    COMPACT_SYM = 648,             /* COMPACT_SYM  */
    COMPLETION_SYM = 649,          /* COMPLETION_SYM  */
    COMPRESSED_SYM = 650,          /* COMPRESSED_SYM  */
    CONCURRENT = 651,              /* CONCURRENT  */
    CONNECTION_SYM = 652,          /* CONNECTION_SYM  */
    CONSISTENT_SYM = 653,          /* CONSISTENT_SYM  */
    CONSTRAINT_CATALOG_SYM = 654,  /* CONSTRAINT_CATALOG_SYM  */
    CONSTRAINT_NAME_SYM = 655,     /* CONSTRAINT_NAME_SYM  */
    CONSTRAINT_SCHEMA_SYM = 656,   /* CONSTRAINT_SCHEMA_SYM  */
    CONTAINS_SYM = 657,            /* CONTAINS_SYM  */
    CONTEXT_SYM = 658,             /* CONTEXT_SYM  */
    CONTRIBUTORS_SYM = 659,        /* CONTRIBUTORS_SYM  */
    CONVERSION_SYM = 660,          /* CONVERSION_SYM  */
    CPU_SYM = 661,                 /* CPU_SYM  */
    CUBE_SYM = 662,                /* CUBE_SYM  */
    CURRENT_SYM = 663,             /* CURRENT_SYM  */
    CURRENT_POS_SYM = 664,         /* CURRENT_POS_SYM  */
    CURSOR_NAME_SYM = 665,         /* CURSOR_NAME_SYM  */
    CYCLE_SYM = 666,               /* CYCLE_SYM  */
    DATA_SYM = 667,                /* DATA_SYM  */
    DATETIME = 668,                /* DATETIME  */
    DATE_SYM = 669,                /* DATE_SYM  */
    DAY_SYM = 670,                 /* DAY_SYM  */
    DEALLOCATE_SYM = 671,          /* DEALLOCATE_SYM  */
    DEFINER_SYM = 672,             /* DEFINER_SYM  */
    DELAYED_SYM = 673,             /* DELAYED_SYM  */
    DELAY_KEY_WRITE_SYM = 674,     /* DELAY_KEY_WRITE_SYM  */
    DIAGNOSTICS_SYM = 675,         /* DIAGNOSTICS_SYM  */
    DIRECTORY_SYM = 676,           /* DIRECTORY_SYM  */
    DISABLE_SYM = 677,             /* DISABLE_SYM  */
    DISCARD = 678,                 /* DISCARD  */
    DISK_SYM = 679,                /* DISK_SYM  */
    DO_SYM = 680,                  /* DO_SYM  */
    DUMPFILE = 681,                /* DUMPFILE  */
    DUPLICATE_SYM = 682,           /* DUPLICATE_SYM  */
    DYNAMIC_SYM = 683,             /* DYNAMIC_SYM  */
    ENABLE_SYM = 684,              /* ENABLE_SYM  */
    END = 685,                     /* END  */
    ENDS_SYM = 686,                /* ENDS_SYM  */
    ENGINES_SYM = 687,             /* ENGINES_SYM  */
    ENGINE_SYM = 688,              /* ENGINE_SYM  */
    ENUM = 689,                    /* ENUM  */
    ERROR_SYM = 690,               /* ERROR_SYM  */
    ERRORS = 691,                  /* ERRORS  */
    ESCAPE_SYM = 692,              /* ESCAPE_SYM  */
    EVENTS_SYM = 693,              /* EVENTS_SYM  */
    EVENT_SYM = 694,               /* EVENT_SYM  */
    EVERY_SYM = 695,               /* EVERY_SYM  */
    EXCHANGE_SYM = 696,            /* EXCHANGE_SYM  */
    EXAMINED_SYM = 697,            /* EXAMINED_SYM  */
    EXCLUDE_SYM = 698,             /* EXCLUDE_SYM  */
    EXECUTE_SYM = 699,             /* EXECUTE_SYM  */
    EXCEPTION_MARIADB_SYM = 700,   /* EXCEPTION_MARIADB_SYM  */
    EXIT_MARIADB_SYM = 701,        /* EXIT_MARIADB_SYM  */
    EXIT_ORACLE_SYM = 702,         /* EXIT_ORACLE_SYM  */
    EXPANSION_SYM = 703,           /* EXPANSION_SYM  */
    EXPIRE_SYM = 704,              /* EXPIRE_SYM  */
    EXPORT_SYM = 705,              /* EXPORT_SYM  */
    EXTENDED_SYM = 706,            /* EXTENDED_SYM  */
    EXTENT_SIZE_SYM = 707,         /* EXTENT_SIZE_SYM  */
    FAST_SYM = 708,                /* FAST_SYM  */
    FAULTS_SYM = 709,              /* FAULTS_SYM  */
    FEDERATED_SYM = 710,           /* FEDERATED_SYM  */
    FILE_SYM = 711,                /* FILE_SYM  */
    FIRST_SYM = 712,               /* FIRST_SYM  */
    FIXED_SYM = 713,               /* FIXED_SYM  */
    FLUSH_SYM = 714,               /* FLUSH_SYM  */
    FOLLOWS_SYM = 715,             /* FOLLOWS_SYM  */
    FOLLOWING_SYM = 716,           /* FOLLOWING_SYM  */
    FORCE_SYM = 717,               /* FORCE_SYM  */
    FORMAT_SYM = 718,              /* FORMAT_SYM  */
    FOUND_SYM = 719,               /* FOUND_SYM  */
    FULL = 720,                    /* FULL  */
    FUNCTION_SYM = 721,            /* FUNCTION_SYM  */
    GENERAL = 722,                 /* GENERAL  */
    GENERATED_SYM = 723,           /* GENERATED_SYM  */
    GET_FORMAT = 724,              /* GET_FORMAT  */
    GET_SYM = 725,                 /* GET_SYM  */
    GLOBAL_SYM = 726,              /* GLOBAL_SYM  */
    GRANTS = 727,                  /* GRANTS  */
    HANDLER_SYM = 728,             /* HANDLER_SYM  */
    HARD_SYM = 729,                /* HARD_SYM  */
    HASH_SYM = 730,                /* HASH_SYM  */
    HELP_SYM = 731,                /* HELP_SYM  */
    HIGH_PRIORITY = 732,           /* HIGH_PRIORITY  */
    HISTORY_SYM = 733,             /* HISTORY_SYM  */
    HOST_SYM = 734,                /* HOST_SYM  */
    HOSTS_SYM = 735,               /* HOSTS_SYM  */
    HOUR_SYM = 736,                /* HOUR_SYM  */
    ID_SYM = 737,                  /* ID_SYM  */
    IDENTIFIED_SYM = 738,          /* IDENTIFIED_SYM  */
    IGNORE_SERVER_IDS_SYM = 739,   /* IGNORE_SERVER_IDS_SYM  */
    IMMEDIATE_SYM = 740,           /* IMMEDIATE_SYM  */
    IMPORT = 741,                  /* IMPORT  */
    INCREMENT_SYM = 742,           /* INCREMENT_SYM  */
    INDEXES = 743,                 /* INDEXES  */
    INSERT_METHOD = 744,           /* INSERT_METHOD  */
    INSTALL_SYM = 745,             /* INSTALL_SYM  */
    INVOKER_SYM = 746,             /* INVOKER_SYM  */
    IO_SYM = 747,                  /* IO_SYM  */
    IPC_SYM = 748,                 /* IPC_SYM  */
    ISOLATION = 749,               /* ISOLATION  */
    ISOPEN_SYM = 750,              /* ISOPEN_SYM  */
    ISSUER_SYM = 751,              /* ISSUER_SYM  */
    INVISIBLE_SYM = 752,           /* INVISIBLE_SYM  */
    JSON_SYM = 753,                /* JSON_SYM  */
    KEY_BLOCK_SIZE = 754,          /* KEY_BLOCK_SIZE  */
    LANGUAGE_SYM = 755,            /* LANGUAGE_SYM  */
    LAST_SYM = 756,                /* LAST_SYM  */
    LAST_VALUE = 757,              /* LAST_VALUE  */
    LASTVAL_SYM = 758,             /* LASTVAL_SYM  */
    LEAVES = 759,                  /* LEAVES  */
    LESS_SYM = 760,                /* LESS_SYM  */
    LEVEL_SYM = 761,               /* LEVEL_SYM  */
    LIST_SYM = 762,                /* LIST_SYM  */
    LOCAL_SYM = 763,               /* LOCAL_SYM  */
    LOCKED_SYM = 764,              /* LOCKED_SYM  */
    LOCKS_SYM = 765,               /* LOCKS_SYM  */
    LOGS_SYM = 766,                /* LOGS_SYM  */
    MASTER_CONNECT_RETRY_SYM = 767, /* MASTER_CONNECT_RETRY_SYM  */
    MASTER_DELAY_SYM = 768,        /* MASTER_DELAY_SYM  */
    MASTER_GTID_POS_SYM = 769,     /* MASTER_GTID_POS_SYM  */
    MASTER_HOST_SYM = 770,         /* MASTER_HOST_SYM  */
    MASTER_LOG_FILE_SYM = 771,     /* MASTER_LOG_FILE_SYM  */
    MASTER_LOG_POS_SYM = 772,      /* MASTER_LOG_POS_SYM  */
    MASTER_PASSWORD_SYM = 773,     /* MASTER_PASSWORD_SYM  */
    MASTER_PORT_SYM = 774,         /* MASTER_PORT_SYM  */
    MASTER_RETRY_COUNT_SYM = 775,  /* MASTER_RETRY_COUNT_SYM  */
    MASTER_SERVER_ID_SYM = 776,    /* MASTER_SERVER_ID_SYM  */
    MASTER_SSL_CAPATH_SYM = 777,   /* MASTER_SSL_CAPATH_SYM  */
    MASTER_SSL_CA_SYM = 778,       /* MASTER_SSL_CA_SYM  */
    MASTER_SSL_CERT_SYM = 779,     /* MASTER_SSL_CERT_SYM  */
    MASTER_SSL_CIPHER_SYM = 780,   /* MASTER_SSL_CIPHER_SYM  */
    MASTER_SSL_CRL_SYM = 781,      /* MASTER_SSL_CRL_SYM  */
    MASTER_SSL_CRLPATH_SYM = 782,  /* MASTER_SSL_CRLPATH_SYM  */
    MASTER_SSL_KEY_SYM = 783,      /* MASTER_SSL_KEY_SYM  */
    MASTER_SSL_SYM = 784,          /* MASTER_SSL_SYM  */
    MASTER_SYM = 785,              /* MASTER_SYM  */
    MASTER_USER_SYM = 786,         /* MASTER_USER_SYM  */
    MASTER_USE_GTID_SYM = 787,     /* MASTER_USE_GTID_SYM  */
    MASTER_HEARTBEAT_PERIOD_SYM = 788, /* MASTER_HEARTBEAT_PERIOD_SYM  */
    MASTER_DEMOTE_TO_SLAVE_SYM = 789, /* MASTER_DEMOTE_TO_SLAVE_SYM  */
    MAX_CONNECTIONS_PER_HOUR = 790, /* MAX_CONNECTIONS_PER_HOUR  */
    MAX_QUERIES_PER_HOUR = 791,    /* MAX_QUERIES_PER_HOUR  */
    MAX_ROWS = 792,                /* MAX_ROWS  */
    MAX_UPDATES_PER_HOUR = 793,    /* MAX_UPDATES_PER_HOUR  */
    MAX_STATEMENT_TIME_SYM = 794,  /* MAX_STATEMENT_TIME_SYM  */
    MAX_USER_CONNECTIONS_SYM = 795, /* MAX_USER_CONNECTIONS_SYM  */
    MEDIUM_SYM = 796,              /* MEDIUM_SYM  */
    MEMORY_SYM = 797,              /* MEMORY_SYM  */
    MERGE_SYM = 798,               /* MERGE_SYM  */
    MESSAGE_TEXT_SYM = 799,        /* MESSAGE_TEXT_SYM  */
    MICROSECOND_SYM = 800,         /* MICROSECOND_SYM  */
    MIGRATE_SYM = 801,             /* MIGRATE_SYM  */
    MINUTE_SYM = 802,              /* MINUTE_SYM  */
    MINVALUE_SYM = 803,            /* MINVALUE_SYM  */
    MIN_ROWS = 804,                /* MIN_ROWS  */
    MODE_SYM = 805,                /* MODE_SYM  */
    MODIFY_SYM = 806,              /* MODIFY_SYM  */
    MONITOR_SYM = 807,             /* MONITOR_SYM  */
    MONTH_SYM = 808,               /* MONTH_SYM  */
    MUTEX_SYM = 809,               /* MUTEX_SYM  */
    MYSQL_SYM = 810,               /* MYSQL_SYM  */
    MYSQL_ERRNO_SYM = 811,         /* MYSQL_ERRNO_SYM  */
    NAMES_SYM = 812,               /* NAMES_SYM  */
    NAME_SYM = 813,                /* NAME_SYM  */
    NATIONAL_SYM = 814,            /* NATIONAL_SYM  */
    NCHAR_SYM = 815,               /* NCHAR_SYM  */
    NEVER_SYM = 816,               /* NEVER_SYM  */
    NEXT_SYM = 817,                /* NEXT_SYM  */
    NEXTVAL_SYM = 818,             /* NEXTVAL_SYM  */
    NOCACHE_SYM = 819,             /* NOCACHE_SYM  */
    NOCYCLE_SYM = 820,             /* NOCYCLE_SYM  */
    NODEGROUP_SYM = 821,           /* NODEGROUP_SYM  */
    NONE_SYM = 822,                /* NONE_SYM  */
    NOTFOUND_SYM = 823,            /* NOTFOUND_SYM  */
    NO_SYM = 824,                  /* NO_SYM  */
    NOMAXVALUE_SYM = 825,          /* NOMAXVALUE_SYM  */
    NOMINVALUE_SYM = 826,          /* NOMINVALUE_SYM  */
    NOWAIT_SYM = 827,              /* NOWAIT_SYM  */
    NUMBER_MARIADB_SYM = 828,      /* NUMBER_MARIADB_SYM  */
    NUMBER_ORACLE_SYM = 829,       /* NUMBER_ORACLE_SYM  */
    NVARCHAR_SYM = 830,            /* NVARCHAR_SYM  */
    OBJECT_SYM = 831,              /* OBJECT_SYM  */
    OF_SYM = 832,                  /* OF_SYM  */
    OFFSET_SYM = 833,              /* OFFSET_SYM  */
    OLD_PASSWORD_SYM = 834,        /* OLD_PASSWORD_SYM  */
    ONE_SYM = 835,                 /* ONE_SYM  */
    ONLY_SYM = 836,                /* ONLY_SYM  */
    ONLINE_SYM = 837,              /* ONLINE_SYM  */
    OPEN_SYM = 838,                /* OPEN_SYM  */
    OPTIONS_SYM = 839,             /* OPTIONS_SYM  */
    OPTION = 840,                  /* OPTION  */
    OVERLAPS_SYM = 841,            /* OVERLAPS_SYM  */
    OWNER_SYM = 842,               /* OWNER_SYM  */
    PACK_KEYS_SYM = 843,           /* PACK_KEYS_SYM  */
    PAGE_SYM = 844,                /* PAGE_SYM  */
    PARSER_SYM = 845,              /* PARSER_SYM  */
    PARTIAL = 846,                 /* PARTIAL  */
    PARTITIONS_SYM = 847,          /* PARTITIONS_SYM  */
    PARTITIONING_SYM = 848,        /* PARTITIONING_SYM  */
    PASSWORD_SYM = 849,            /* PASSWORD_SYM  */
    PERIOD_SYM = 850,              /* PERIOD_SYM  */
    PERSISTENT_SYM = 851,          /* PERSISTENT_SYM  */
    PHASE_SYM = 852,               /* PHASE_SYM  */
    PLUGINS_SYM = 853,             /* PLUGINS_SYM  */
    PLUGIN_SYM = 854,              /* PLUGIN_SYM  */
    PORT_SYM = 855,                /* PORT_SYM  */
    PRECEDES_SYM = 856,            /* PRECEDES_SYM  */
    PRECEDING_SYM = 857,           /* PRECEDING_SYM  */
    PREPARE_SYM = 858,             /* PREPARE_SYM  */
    PRESERVE_SYM = 859,            /* PRESERVE_SYM  */
    PREV_SYM = 860,                /* PREV_SYM  */
    PREVIOUS_SYM = 861,            /* PREVIOUS_SYM  */
    PRIVILEGES = 862,              /* PRIVILEGES  */
    PROCESS = 863,                 /* PROCESS  */
    PROCESSLIST_SYM = 864,         /* PROCESSLIST_SYM  */
    PROFILE_SYM = 865,             /* PROFILE_SYM  */
    PROFILES_SYM = 866,            /* PROFILES_SYM  */
    PROXY_SYM = 867,               /* PROXY_SYM  */
    QUARTER_SYM = 868,             /* QUARTER_SYM  */
    QUERY_SYM = 869,               /* QUERY_SYM  */
    QUICK = 870,                   /* QUICK  */
    RAW_MARIADB_SYM = 871,         /* RAW_MARIADB_SYM  */
    RAW_ORACLE_SYM = 872,          /* RAW_ORACLE_SYM  */
    READ_ONLY_SYM = 873,           /* READ_ONLY_SYM  */
    REBUILD_SYM = 874,             /* REBUILD_SYM  */
    RECOVER_SYM = 875,             /* RECOVER_SYM  */
    REDUNDANT_SYM = 876,           /* REDUNDANT_SYM  */
    RELAY = 877,                   /* RELAY  */
    RELAYLOG_SYM = 878,            /* RELAYLOG_SYM  */
    RELAY_LOG_FILE_SYM = 879,      /* RELAY_LOG_FILE_SYM  */
    RELAY_LOG_POS_SYM = 880,       /* RELAY_LOG_POS_SYM  */
    RELAY_THREAD = 881,            /* RELAY_THREAD  */
    RELOAD = 882,                  /* RELOAD  */
    REMOVE_SYM = 883,              /* REMOVE_SYM  */
    REORGANIZE_SYM = 884,          /* REORGANIZE_SYM  */
    REPAIR = 885,                  /* REPAIR  */
    REPEATABLE_SYM = 886,          /* REPEATABLE_SYM  */
    REPLAY_SYM = 887,              /* REPLAY_SYM  */
    REPLICATION = 888,             /* REPLICATION  */
    RESET_SYM = 889,               /* RESET_SYM  */
    RESTART_SYM = 890,             /* RESTART_SYM  */
    RESOURCES = 891,               /* RESOURCES  */
    RESUME_SYM = 892,              /* RESUME_SYM  */
    RETURNED_SQLSTATE_SYM = 893,   /* RETURNED_SQLSTATE_SYM  */
    RETURNS_SYM = 894,             /* RETURNS_SYM  */
    REUSE_SYM = 895,               /* REUSE_SYM  */
    REVERSE_SYM = 896,             /* REVERSE_SYM  */
    ROLE_SYM = 897,                /* ROLE_SYM  */
    ROLLBACK_SYM = 898,            /* ROLLBACK_SYM  */
    ROLLUP_SYM = 899,              /* ROLLUP_SYM  */
    ROUTINE_SYM = 900,             /* ROUTINE_SYM  */
    ROWCOUNT_SYM = 901,            /* ROWCOUNT_SYM  */
    ROW_SYM = 902,                 /* ROW_SYM  */
    ROW_COUNT_SYM = 903,           /* ROW_COUNT_SYM  */
    ROW_FORMAT_SYM = 904,          /* ROW_FORMAT_SYM  */
    RTREE_SYM = 905,               /* RTREE_SYM  */
    SAVEPOINT_SYM = 906,           /* SAVEPOINT_SYM  */
    SCALAR_SYM = 907,              /* SCALAR_SYM  */
    SCHEDULE_SYM = 908,            /* SCHEDULE_SYM  */
    SCHEMA_NAME_SYM = 909,         /* SCHEMA_NAME_SYM  */
    SECOND_SYM = 910,              /* SECOND_SYM  */
    SECURITY_SYM = 911,            /* SECURITY_SYM  */
    SEQUENCE_SYM = 912,            /* SEQUENCE_SYM  */
    SERIALIZABLE_SYM = 913,        /* SERIALIZABLE_SYM  */
    SERIAL_SYM = 914,              /* SERIAL_SYM  */
    SESSION_SYM = 915,             /* SESSION_SYM  */
    SESSION_USER_SYM = 916,        /* SESSION_USER_SYM  */
    SERVER_SYM = 917,              /* SERVER_SYM  */
    SETVAL_SYM = 918,              /* SETVAL_SYM  */
    SHARE_SYM = 919,               /* SHARE_SYM  */
    SHUTDOWN = 920,                /* SHUTDOWN  */
    SIGNED_SYM = 921,              /* SIGNED_SYM  */
    SIMPLE_SYM = 922,              /* SIMPLE_SYM  */
    SKIP_SYM = 923,                /* SKIP_SYM  */
    SLAVE = 924,                   /* SLAVE  */
    SLAVES = 925,                  /* SLAVES  */
    SLAVE_POS_SYM = 926,           /* SLAVE_POS_SYM  */
    SLOW = 927,                    /* SLOW  */
    SNAPSHOT_SYM = 928,            /* SNAPSHOT_SYM  */
    SOCKET_SYM = 929,              /* SOCKET_SYM  */
    SOFT_SYM = 930,                /* SOFT_SYM  */
    SONAME_SYM = 931,              /* SONAME_SYM  */
    SOUNDS_SYM = 932,              /* SOUNDS_SYM  */
    SOURCE_SYM = 933,              /* SOURCE_SYM  */
    SQL_AFTER_GTIDS_SYM = 934,     /* SQL_AFTER_GTIDS_SYM  */
    SQL_BEFORE_GTIDS_SYM = 935,    /* SQL_BEFORE_GTIDS_SYM  */
    SQL_BUFFER_RESULT = 936,       /* SQL_BUFFER_RESULT  */
    SQL_CACHE_SYM = 937,           /* SQL_CACHE_SYM  */
    SQL_CALC_FOUND_ROWS = 938,     /* SQL_CALC_FOUND_ROWS  */
    SQL_NO_CACHE_SYM = 939,        /* SQL_NO_CACHE_SYM  */
    SQL_THREAD = 940,              /* SQL_THREAD  */
    STAGE_SYM = 941,               /* STAGE_SYM  */
    STARTS_SYM = 942,              /* STARTS_SYM  */
    START_SYM = 943,               /* START_SYM  */
    STATEMENT_SYM = 944,           /* STATEMENT_SYM  */
    STATUS_SYM = 945,              /* STATUS_SYM  */
    STOP_SYM = 946,                /* STOP_SYM  */
    STORAGE_SYM = 947,             /* STORAGE_SYM  */
    STORED_SYM = 948,              /* STORED_SYM  */
    STRING_SYM = 949,              /* STRING_SYM  */
    SUBCLASS_ORIGIN_SYM = 950,     /* SUBCLASS_ORIGIN_SYM  */
    SUBDATE_SYM = 951,             /* SUBDATE_SYM  */
    SUBJECT_SYM = 952,             /* SUBJECT_SYM  */
    SUBPARTITIONS_SYM = 953,       /* SUBPARTITIONS_SYM  */
    SUBPARTITION_SYM = 954,        /* SUBPARTITION_SYM  */
    SUPER_SYM = 955,               /* SUPER_SYM  */
    SUSPEND_SYM = 956,             /* SUSPEND_SYM  */
    SWAPS_SYM = 957,               /* SWAPS_SYM  */
    SWITCHES_SYM = 958,            /* SWITCHES_SYM  */
    SYSTEM = 959,                  /* SYSTEM  */
    SYSTEM_TIME_SYM = 960,         /* SYSTEM_TIME_SYM  */
    TABLES = 961,                  /* TABLES  */
    TABLESPACE = 962,              /* TABLESPACE  */
    TABLE_CHECKSUM_SYM = 963,      /* TABLE_CHECKSUM_SYM  */
    TABLE_NAME_SYM = 964,          /* TABLE_NAME_SYM  */
    TEMPORARY = 965,               /* TEMPORARY  */
    TEMPTABLE_SYM = 966,           /* TEMPTABLE_SYM  */
    TEXT_SYM = 967,                /* TEXT_SYM  */
    THAN_SYM = 968,                /* THAN_SYM  */
    TIES_SYM = 969,                /* TIES_SYM  */
    TIMESTAMP = 970,               /* TIMESTAMP  */
    TIMESTAMP_ADD = 971,           /* TIMESTAMP_ADD  */
    TIMESTAMP_DIFF = 972,          /* TIMESTAMP_DIFF  */
    TIME_SYM = 973,                /* TIME_SYM  */
    TRANSACTION_SYM = 974,         /* TRANSACTION_SYM  */
    TRANSACTIONAL_SYM = 975,       /* TRANSACTIONAL_SYM  */
    THREADS_SYM = 976,             /* THREADS_SYM  */
    TO_DATE = 977,                 /* TO_DATE  */
    TRIGGERS_SYM = 978,            /* TRIGGERS_SYM  */
    TRIM_ORACLE = 979,             /* TRIM_ORACLE  */
    TRUNCATE_SYM = 980,            /* TRUNCATE_SYM  */
    TYPE_SYM = 981,                /* TYPE_SYM  */
    UNBOUNDED_SYM = 982,           /* UNBOUNDED_SYM  */
    UNCOMMITTED_SYM = 983,         /* UNCOMMITTED_SYM  */
    UNDEFINED_SYM = 984,           /* UNDEFINED_SYM  */
    UNICODE_SYM = 985,             /* UNICODE_SYM  */
    UNINSTALL_SYM = 986,           /* UNINSTALL_SYM  */
    UNKNOWN_SYM = 987,             /* UNKNOWN_SYM  */
    UNTIL_SYM = 988,               /* UNTIL_SYM  */
    UPGRADE_SYM = 989,             /* UPGRADE_SYM  */
    USER_SYM = 990,                /* USER_SYM  */
    USE_FRM = 991,                 /* USE_FRM  */
    VALIDATION_SYM = 992,          /* VALIDATION_SYM  */
    VALUE_SYM = 993,               /* VALUE_SYM  */
    VARCHAR2_MARIADB_SYM = 994,    /* VARCHAR2_MARIADB_SYM  */
    VARCHAR2_ORACLE_SYM = 995,     /* VARCHAR2_ORACLE_SYM  */
    VARIABLES = 996,               /* VARIABLES  */
    VERSIONING_SYM = 997,          /* VERSIONING_SYM  */
    VIA_SYM = 998,                 /* VIA_SYM  */
    VIEW_SYM = 999,                /* VIEW_SYM  */
    VISIBLE_SYM = 1000,            /* VISIBLE_SYM  */
    VIRTUAL_SYM = 1001,            /* VIRTUAL_SYM  */
    WAIT_SYM = 1002,               /* WAIT_SYM  */
    WARNINGS = 1003,               /* WARNINGS  */
    WEEK_SYM = 1004,               /* WEEK_SYM  */
    WEIGHT_STRING_SYM = 1005,      /* WEIGHT_STRING_SYM  */
    WINDOW_SYM = 1006,             /* WINDOW_SYM  */
    WITHIN = 1007,                 /* WITHIN  */
    WITHOUT = 1008,                /* WITHOUT  */
    WORK_SYM = 1009,               /* WORK_SYM  */
    WRAPPER_SYM = 1010,            /* WRAPPER_SYM  */
    WRITE_SYM = 1011,              /* WRITE_SYM  */
    X509_SYM = 1012,               /* X509_SYM  */
    XA_SYM = 1013,                 /* XA_SYM  */
    XML_SYM = 1014,                /* XML_SYM  */
    YEAR_SYM = 1015,               /* YEAR_SYM  */
    ST_COLLECT_SYM = 1016,         /* ST_COLLECT_SYM  */
    CONDITIONLESS_JOIN = 1017,     /* CONDITIONLESS_JOIN  */
    ON_SYM = 1018,                 /* ON_SYM  */
    PREC_BELOW_NOT = 1019,         /* PREC_BELOW_NOT  */
    SUBQUERY_AS_EXPR = 1020,       /* SUBQUERY_AS_EXPR  */
    PREC_BELOW_IDENTIFIER_OPT_SPECIAL_CASE = 1021, /* PREC_BELOW_IDENTIFIER_OPT_SPECIAL_CASE  */
    USER = 1022,                   /* USER  */
    PREC_BELOW_SP_OBJECT_TYPE = 1023, /* PREC_BELOW_SP_OBJECT_TYPE  */
    PREC_BELOW_CONTRACTION_TOKEN2 = 1024, /* PREC_BELOW_CONTRACTION_TOKEN2  */
    EMPTY_FROM_CLAUSE = 1025       /* EMPTY_FROM_CLAUSE  */
  };
  typedef enum yytokentype yytoken_kind_t;
#endif

/* Value type.  */
#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
union YYSTYPE
{
#line 202 "/Users/aryankancherla/Downloads/DLAB_MariaDB/server/sql/sql_yacc.yy"

  int  num;
  ulong ulong_num;
  ulonglong ulonglong_number;
  longlong longlong_number;
  uint sp_instr_addr;
  /*
    Longlong_hybrid does not have a default constructor, hence the
    default value below.
  */
  Longlong_hybrid longlong_hybrid_number= Longlong_hybrid(0, false);

  /* structs */
  /**
    This is a stand-in for @ref std::optional,
    which is not trivially `union`-safe.
  */
  struct
  {
    bool has_value; uint64_t value;
#if __cplusplus >= 201703L || _MSVC_LANG >= 201703L
    template<typename I> operator std::optional<I>()
    { return has_value ? std::optional(static_cast<I>(value)) : std::nullopt; }
#endif
  } optional_uint;
  LEX_CSTRING lex_str;
  Lex_comment_st lex_comment;
  Lex_ident_cli_st kwd;
  Lex_ident_cli_st ident_cli;
  Lex_ident_sys_st ident_sys;
  Lex_column_list_privilege_st column_list_privilege;
  Lex_string_with_metadata_st lex_string_with_metadata;
  Lex_spblock_st spblock;
  Lex_spblock_handlers_st spblock_handlers;
  Lex_length_and_dec_st Lex_length_and_dec;
  Lex_cast_type_st Lex_cast_type;
  Lex_field_type_st Lex_field_type;
  Lex_exact_charset_extended_collation_attrs_st
                    Lex_exact_charset_extended_collation_attrs;
  Lex_extended_collation_st Lex_extended_collation;
  Lex_dyncol_type_st Lex_dyncol_type;
  Lex_for_loop_st for_loop;
  Lex_for_loop_bounds_st for_loop_bounds;
  Lex_trim_st trim;
  Json_table_column::On_response json_on_response;
  Lex_substring_spec_st substring_spec;
  vers_history_point_t vers_history_point;
  struct
  {
    enum sub_select_type unit_type;
    bool distinct;
  } unit_operation;
  struct
  {
    SELECT_LEX *first;
    SELECT_LEX *prev_last;
  } select_list;
  SQL_I_List<ORDER> *select_order;
  Lex_select_lock select_lock;
  Lex_select_limit select_limit;
  Lex_order_limit_lock *order_limit_lock;
  struct {
    bool with_unique_keys;
    ulong type_constraint;
  } json_predicate;

  /* pointers */
  Lex_ident_sys *ident_sys_ptr;
  Create_field *create_field;
  Spvar_definition *spvar_definition;
  Row_definition_list *spvar_definition_list;
  const Type_handler *type_handler;
  const class Sp_handler *sp_handler;
  CHARSET_INFO *charset;
  Condition_information_item *cond_info_item;
  DYNCALL_CREATE_DEF *dyncol_def;
  Diagnostics_information *diag_info;
  Item *item;
  Item_num *item_num;
  Item_param *item_param;
  Item_basic_constant *item_basic_constant;
  Key_part_spec *key_part;
  LEX *lex;
  sp_instr_fetch_cursor *instr_fetch_cursor;
  sp_expr_lex *expr_lex;
  sp_assignment_lex *assignment_lex;
  class sp_lex_cursor *sp_cursor_stmt;
  LEX_CSTRING *lex_str_ptr;
  LEX_USER *lex_user;
  USER_AUTH *user_auth;
  List<Condition_information_item> *cond_info_list;
  List<DYNCALL_CREATE_DEF> *dyncol_def_list;
  List<Item> *item_list;
  List_sp_assignment_lex *sp_assignment_lex_list;
  List<Statement_information_item> *stmt_info_list;
  List<String> *string_list;
  List<Lex_ident_sys> *ident_sys_list;
  List<sp_fetch_target> *fetch_target_list;
  Statement_information_item *stmt_info_item;
  String *string;
  TABLE_LIST *table_list;
  Table_ident *table;
  Qualified_column_ident *qualified_column_ident;
  Optimizer_hint_parser_output *opt_hints;
  Qualified_ident *qualified_ident;
  char *simple_string;
  const char *const_simple_string;
  chooser_compare_func_creator boolfunc2creator;
  class Lex_grant_privilege *lex_grant;
  class Lex_grant_object_name *lex_grant_ident;
  class my_var *myvar;
  class sp_condition_value *spcondvalue;
  class sp_head *sphead;
  class sp_name *spname;
  class sp_variable *spvar;
  class sp_type_def_record *sprec;
  class With_element_head *with_element_head;
  class With_clause *with_clause;
  class Virtual_column_info *virtual_column;
  engine_option_value *engine_option_value_ptr;

  handlerton *db_type;
  st_select_lex *select_lex;
  st_select_lex_unit *select_lex_unit;
  struct p_elem_val *p_elem_value;
  class Window_frame *window_frame;
  class Window_frame_bound *window_frame_bound;
  udf_func *udf;
  st_trg_execution_order trg_execution_order;

  /* enums */
  trilean tril;
  enum enum_sp_suid_behaviour sp_suid;
  enum enum_sp_aggregate_type sp_aggregate_type;
  enum enum_view_suid view_suid;
  enum Condition_information_item::Name cond_info_item_name;
  enum enum_diag_condition_item_name diag_condition_item_name;
  enum Diagnostics_information::Which_area diag_area;
  enum enum_fk_option m_fk_option;
  enum Item_udftype udf_type;
  enum Key::Keytype key_type;
  enum Statement_information_item::Name stmt_info_item_name;
  enum enum_filetype filetype;
  enum enum_tx_isolation tx_isolation;
  enum enum_var_type var_type;
  enum enum_yes_no_unknown m_yes_no_unk;
  enum ha_choice choice;
  enum ha_key_alg key_alg;
  enum ha_rkey_function ha_rkey_mode;
  enum index_hint_type index_hint;
  enum interval_type interval, interval_time_st;
  enum row_type row_type;
  enum sp_variable::enum_mode spvar_mode;
  enum thr_lock_type lock_type;
  enum enum_mysql_timestamp_type date_time_type;
  enum Window_frame_bound::Bound_precedence_type bound_precedence_type;
  enum Window_frame::Frame_units frame_units;
  enum Window_frame::Frame_exclusion frame_exclusion;
  enum trigger_order_type trigger_action_order_type;
  DDL_options_st object_ddl_options;
  enum vers_kind_t vers_range_unit;
  enum Column_definition::enum_column_versioning vers_column_versioning;
  enum plsql_cursor_attr_t plsql_cursor_attr;
  enum Alter_info::enum_alter_table_algorithm alter_table_algo_val;
  enum_master_use_gtid master_use_gtid;
  privilege_t privilege;
  struct
  {
    Item *expr;
    LEX_CSTRING expr_str;
  } expr_and_query_str;

#line 1014 "/Users/aryankancherla/Downloads/DLAB_MariaDB/server/mariadb-build/sql/yy_mariadb.hh"

};
typedef union YYSTYPE YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define YYSTYPE_IS_DECLARED 1
#endif




int MYSQLparse (THD *thd);


#endif /* !YY_MYSQL_USERS_ARYANKANCHERLA_DOWNLOADS_DLAB_MARIADB_SERVER_MARIADB_BUILD_SQL_YY_MARIADB_HH_INCLUDED  */
