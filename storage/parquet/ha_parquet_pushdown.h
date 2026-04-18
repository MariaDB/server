#ifndef HA_PARQUET_PUSHDOWN_INCLUDED
#define HA_PARQUET_PUSHDOWN_INCLUDED

class THD;
typedef class st_select_lex SELECT_LEX;
typedef class st_select_lex_unit SELECT_LEX_UNIT;
class select_handler;

select_handler *create_duckdb_select_handler(THD *thd,
                                             SELECT_LEX *sel_lex,
                                             SELECT_LEX_UNIT *sel_unit);

#endif
