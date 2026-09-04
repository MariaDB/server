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

  /* Usage: Construct the fulltext predicate implementing this access */
  Item *create_ft_item(THD *thd);
};

bool setup_mvi_for_join(JOIN *join);

/* Return the compatible json type */
enum json_value_types mvi_json_class(enum_field_types ftype);

bool setup_mvi_quick(JOIN *join);
