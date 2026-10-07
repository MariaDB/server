#ifndef ITEM_COPY_INCLUDED
#define ITEM_COPY_INCLUDED
/*
   Copyright (c) 2026, MariaDB

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

class THD;
class Item;
class Item_cache;


/*
  The purpose of a copy of an item tree, passed to Item::copy_for().

  Most items are copied the same way for every purpose.  An item whose copy
  depends on the purpose implements do_copy_for() by calling a virtual
  method of the context, and each purpose is a subclass that implements
  that method.

  Item_cache is such an item, see copy_cache().  The item that owns a cache,
  e.g., Item_in_optimizer, stores a new value into it for each row, and
  nothing stores into a copy of the cache.
*/

class Copy_context
{
public:
  virtual ~Copy_context() = default;

  /*
    Return the item that takes the place of cache in the copy, or NULL if
    the tree cannot be copied for this purpose.
  */
  virtual Item *copy_cache(THD *thd, const Item_cache *cache) const= 0;

#ifndef DBUG_OFF
  /* Names the purpose in the notes of Item::check_copy() */
  virtual const char *name() const= 0;
  /* Whether a copy made for this purpose may hold item itself */
  virtual bool may_share(const Item *item) const= 0;
  /*
    Whether a copy made for this purpose holds an item of another class in
    place of item
  */
  virtual bool substitutes(const Item *item) const= 0;
#endif
};


/*
  The copy is evaluated in another select of the same execution, as a
  condition pushed into a derived table is.  The copy holds the original
  cache, so it reads the value the owner of the cache stores for the current
  row.
*/

class Pushdown_copy_context: public Copy_context
{
public:
  Item *copy_cache(THD *thd, const Item_cache *cache) const override;
#ifndef DBUG_OFF
  const char *name() const override { return "pushdown"; }
  bool may_share(const Item *item) const override;
  bool substitutes(const Item *item) const override { return false; }
#endif
};


/*
  The copy is evaluated in the select of the original, and the rewrite cleans
  the copy and fixes it again.  A tree with a cache is not copied, because
  cleaning a cache shared with the original would clear its value.
*/

class Rewrite_copy_context: public Copy_context
{
public:
  Item *copy_cache(THD *thd, const Item_cache *cache) const override
  { return nullptr; }
#ifndef DBUG_OFF
  const char *name() const override { return "rewrite"; }
  bool may_share(const Item *item) const override { return false; }
  bool substitutes(const Item *item) const override { return false; }
#endif
};


/*
  The copy is evaluated by another thread, so it holds no item of the
  original.  A cache is replaced by a copy of the item it reads its value
  from, which the copy evaluates for every row.
*/

class Parallel_copy_context: public Copy_context
{
public:
  Item *copy_cache(THD *thd, const Item_cache *cache) const override;
#ifndef DBUG_OFF
  const char *name() const override { return "parallel"; }
  bool may_share(const Item *item) const override { return false; }
  bool substitutes(const Item *item) const override;
#endif
};


/*
  The copy outlives the statement, as a virtual column expression does.  A
  cache holds a value of one execution, so a tree with a cache is not copied.
*/

class Persistent_copy_context: public Copy_context
{
public:
  Item *copy_cache(THD *thd, const Item_cache *cache) const override
  { return nullptr; }
#ifndef DBUG_OFF
  const char *name() const override { return "persistent"; }
  bool may_share(const Item *item) const override { return false; }
  bool substitutes(const Item *item) const override { return false; }
#endif
};

#endif /* ITEM_COPY_INCLUDED */
