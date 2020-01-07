
file(READ "${IN}" yytmp)

# Comment out sql_mode=DEFAULT rules and directives (e.g. %expect, %type)
string(REPLACE  "/* Start SQL_MODE_DEFAULT_SPECIFIC */"
                "/* Start SQL_MODE_DEFAULT_SPECIFIC"      yytmp "${yytmp}")
string(REPLACE  "/* End SQL_MODE_DEFAULT_SPECIFIC */"
                   "End SQL_MODE_DEFAULT_SPECIFIC */"     yytmp "${yytmp}")

# Uncomment sql_mode=ORACLE rules and directives
string(REPLACE  "/* Start SQL_MODE_ORACLE_SPECIFIC"
                "/* Start SQL_MODE_ORACLE_SPECIFIC */"    yytmp "${yytmp}")
string(REPLACE     "End SQL_MODE_ORACLE_SPECIFIC */"
                "/* End SQL_MODE_ORACLE_SPECIFIC */"      yytmp "${yytmp}")
file(WRITE "${OUT}" "${yytmp}")
