/*

In the current release model, versions are released like the following
(the table shows the year/quarter of the planned GA release):

        Q1              Q2              Q3              Q4
2022    10.7            10.8            10.9            10.10
2023    10.11-LTS       11.0            11.1            11.2
2024    11.3            11.4            11.5            11.6
2025    11.7-LTS        12.0            12.1            12.2
2026    12.3            12.4            12.5            12.6
2027    12.7-LTS        13.0            13.1            13.2
2028    13.3            13.4            13.5            13.6
2029    13.7-LTS        14.0            14.1            14.2
...

A deprecated feature can be removed only when all releases when it's
not deprecated have reached EOL. For example, if something was
deprecated in 11.3, then 10.11 (where it wasn't deprecated) will reach
EOL in Q1 2028 (standard 5 years LTS life time). Meaning, the feature can
be removed in 13.4.

When the release model changes, the table above and templates below
have to be updated.
*/

template<uint V> static inline void check_deprecated_version(void)
{
  static_assert (
     V <= 1004 ? MYSQL_VERSION_ID < 110500 :   /* until 10.4  EOL */
     V <= 1005 ? MYSQL_VERSION_ID < 120100 :   /* until 10.5  EOL */
     V <= 1010 ? MYSQL_VERSION_ID < 120500 :   /* until 10.6  EOL */
     V <= 1106 ? MYSQL_VERSION_ID < 130400 :   /* until 10.11 EOL */
     V == 999999,    /* only for sys_var::do_deprecated_warning() */
     "check_deprecated_version failed"
  );
}

/*
  V is the 2-component 4-digit version where something was deprecated.
  For example, if deprecated in 11.2: warn_deprecated<1102>(thd, "something")
*/
template<uint V> static inline void warn_deprecated(THD *thd,
                                      const char *what, const char *to= NULL)
{
  check_deprecated_version<V>();
  push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                      ER_WARN_DEPRECATED_SYNTAX, ER_THD(thd, to && *to
                         ? ER_WARN_DEPRECATED_SYNTAX
                         : ER_WARN_DEPRECATED_SYNTAX_NO_REPLACEMENT),
                      what, to);
}

template<uint V> static inline void warn_deprecated(const char *what,
                                                    const char *to= NULL)
{
  check_deprecated_version<V>();
  sql_print_warning(to && *to
                    ? "'%s' is deprecated and will be removed in a future release. Please use %s instead"
                    : "'%s' is deprecated and will be removed in a future release",
                    what, to);
}

/* Prevent direct usage of the error that bypasses the template */
#undef ER_WARN_DEPRECATED_SYNTAX
#undef ER_WARN_DEPRECATED_SYNTAX_WITH_VER
#undef ER_WARN_DEPRECATED_SYNTAX_NO_REPLACEMENT
