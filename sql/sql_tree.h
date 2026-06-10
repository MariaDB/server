#ifndef SQL_TREE_INCLUDED
#define SQL_TREE_INCLUDED
/*
   Copyright (c) 2026, MariaDB plc

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

#include <my_tree.h>

/*
  Type-safe wrapper around the C TREE red-black tree.
*/
template<typename T, typename Param=void>
class Tree
{
  TREE m_tree;

  static int free_shim(void *elem, TREE_FREE action, void *)
  {
    if (action == free_free)
      static_cast<T *>(elem)->~T();
    return 0;
  }

  template<typename Ctx>
  struct walk_arg
  {
    int (*fn)(const T &, element_count, Ctx *);
    Ctx *ctx;
  };

  template<typename Ctx>
  static int call_shim(void *elem, element_count cnt, void *arg_)
  {
    auto *arg= static_cast<walk_arg<Ctx> *>(arg_);
    return arg->fn(*static_cast<const T *>(elem), cnt, arg->ctx);
  }

public:
  explicit Tree(qsort_cmp2 fn, Param *extra_arg= nullptr, myf flags= MYF(0))
  {
    init_tree(&m_tree, 0, 0, sizeof(T), fn, free_shim, (void*)extra_arg, flags);
  }
  ~Tree() { delete_tree(&m_tree, 0); }

  void clear() { delete_tree(&m_tree, 0); }

  Tree(const Tree &)= delete;
  Tree &operator=(const Tree &)= delete;

  /* Insert val; returns true on OOM. Duplicate inserts increment the count. */
  bool insert(const T &val)
  {
    return tree_insert(&m_tree, &val, 0, m_tree.custom_arg) == nullptr;
  }

  T *find(const T &key)
  {
    return static_cast<T *>(tree_search(&m_tree, &key, m_tree.custom_arg));
  }

  /*
    Walk all elements in order, calling fn(const T &, element_count, Ctx &).
    fn should return 0 to continue, non-zero to stop.
  */
  template<typename Ctx>
  int walk(int (*fn)(const T &, element_count, Ctx *), Ctx *ctx,
           TREE_WALK dir= left_root_right)
  {
    walk_arg<Ctx> arg= {fn, ctx};
    return tree_walk(&m_tree, call_shim<Ctx>, &arg, dir);
  }

  uint elements() const { return m_tree.elements_in_tree; }
};

#endif /* SQL_TREE_INCLUDED */
