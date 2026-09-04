/*
   Copyright (c) 2026, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

/* An MVI index */
struct Mv_index : public Sql_alloc
{
  Field *vcol;                  /* The hidden vcol of the index */
  uint keyno;                   /* The keyno of the index */
  Mv_index(Field *vcol_arg, uint keyno_arg)
    : vcol(vcol_arg), keyno(keyno_arg) {}
};

/* Access descriptor for a predicate */
struct Mvi_access : public Sql_alloc
{
  Mv_index *index;
  List<String> encoded;         /* encoded element keys */
  bool conjunctive;             /* CONTAINS -> AND, OVERLAPS -> OR */
  Mvi_access(Mv_index *idx, bool conj) : index(idx), conjunctive(conj) {}

  /* Build: Add one encoded element key */
  bool add_key(MEM_ROOT *mem_root, const String *key);

  /* Usage: Build the fulltext query searching for the element keys */
  bool build_ft_query(String *out);
};


/* The result of the MVI analysis of one JOIN */
class Mvi_context : public Sql_alloc
{
 public:
  THD *thd;
  /* All MV indexes in the JOIN */
  List<Mv_index> indexes;
  /* MVI accesses for all eligible predicates in WHERE */
  List<Mvi_access> accesses;
  /* The access we've chosen for each table, indexed by table->tablenr */
  Mvi_access *best[MAX_TABLES];

  Mvi_context(THD *thd_arg) : thd(thd_arg)
  {
    bzero(best, sizeof(best));
  }
};

/* Return the compatible json type */
enum json_value_types mvi_json_class(enum_field_types ftype);

bool setup_mvi_quick(JOIN *join);

/* Create a quick select for the best MVI access to `table', if there is one */
QUICK_SELECT_I *get_best_mvi_access(THD *thd, JOIN *join, TABLE *table);
