#include "mariadb.h"
#include "sql_base.h"

struct table_pos: public Sql_alloc
{
  /*
    Links the tables in the "LEFT JOIN syntax order"
  */
  table_pos *next;
  table_pos *prev;

  /* Tables we have outgoing edges to. Duplicates are possible. */
  List<table_pos> inner_side;

  /* Incoming edges */
  List<table_pos> outer_side;

  TABLE_LIST *table;

  /* Ordinal number of the table in the original FROM clause */
  int order;

  /* TRUE <=> this table is already linked (in the prev<=>next chain) */
  bool processed;

  /* TRUE <=> All tables in outer_side are already linked in prev/next */
  bool outer_processed;

  bool is_outer_of(table_pos *tab)
  {
    List_iterator_fast<table_pos> it(outer_side);
    table_pos *t;
    while((t= it++))
    {
      if (t == tab)
        return TRUE;
    }
    return FALSE;
   }
};

static void init_tables_array(TABLE_LIST *tables,
                              uint n_tables,
                              table_pos *tab)
{
  table_pos *t= tab;
  TABLE_LIST *table= tables;
  uint i= 0;
  DBUG_ENTER("init_tables_array");

  /* TODO: check for nested joins */
  /*
    Create a graph vertex for each table.
    Also note the original order of tables in the FROM clause.
  */
  for (;table; i++, t++, table= table->next_local)
  {
    DBUG_ASSERT(i < n_tables);
    /* TODO: can mix with inner joins? */
    t->next= t->prev=  NULL;
    t->inner_side.empty();
    t->outer_side.empty();
    t->table= table;
    t->order= i;
    t->processed= t->outer_processed= FALSE;
    if (table->on_expr)
    {
      for (uint j= 0, m= 1; j < n_tables; j++, m*= 2)
      {
        table_map ons= table->on_expr->used_tables();
        if ((ons & m) && j != i)
        {
          t->outer_side.push_back(&tab[j]);
          tab[j].inner_side.push_back(t);
        }
      }
    }
  }
  /* TODO: Somehow this assert does not always hold? */
  /*
    DBUG_ASSERT(i == n_tables);
   */
  DBUG_VOID_RETURN;
}

bool setup_outer_join_graph(THD *thd, Item **conds,
                            TABLE_LIST *tables,
                            SQL_I_List<TABLE_LIST> &select_table_list,
                            List<TABLE_LIST> *select_join_list)
{
  DBUG_ENTER("setup_outer_join_graph");
  uint n_tables= select_table_list.elements;

  table_pos *tab= (table_pos *) new(thd->mem_root) table_pos[n_tables];
  init_tables_array(tables, n_tables, tab);

  DBUG_RETURN(FALSE);
}
