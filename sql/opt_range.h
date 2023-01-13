/*
   Copyright (c) 2000, 2010, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */


/* classes to use when handling where clause */

#ifndef _opt_range_h
#define _opt_range_h

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif

#include "records.h"                            /* READ_RECORD */
#include "queues.h"                             /* QUEUE */
#include "filesort.h"                           /* SORT_INFO */

/*
  It is necessary to include set_var.h instead of item.h because there
  are dependencies on include order for set_var.h and item.h. This
  will be resolved later.
*/
#include "sql_class.h"                          // set_var.h: THD
#include "set_var.h"                            /* Item */

class JOIN;
class Item_sum;

struct KEY_PART {
  uint16           key,part;
  /* See KEY_PART_INFO for meaning of the next two: */
  uint16           store_length, length;
  uint8            null_bit;
  /*
    Keypart flags (0 when this structure is used by partition pruning code
    for fake partitioning index description)
  */
  uint8 flag;
  Field            *field;
  Field::imagetype image_type;
};


/**
  A helper function to invert min flags to max flags for DESC key parts.
  It changes NEAR_MIN, NO_MIN_RANGE to NEAR_MAX, NO_MAX_RANGE appropriately
*/

inline uint invert_min_flag(uint min_flag)
{
  uint max_flag_out = min_flag & ~(NEAR_MIN | NO_MIN_RANGE);
  if (min_flag & NEAR_MIN) max_flag_out |= NEAR_MAX;
  if (min_flag & NO_MIN_RANGE) max_flag_out |= NO_MAX_RANGE;
  return max_flag_out;
}


/**
  A helper function to invert max flags to min flags for DESC key parts.
  It changes NEAR_MAX, NO_MAX_RANGE to NEAR_MIN, NO_MIN_RANGE appropriately
*/

inline uint invert_max_flag(uint max_flag)
{
  uint min_flag_out = max_flag & ~(NEAR_MAX | NO_MAX_RANGE);
  if (max_flag & NEAR_MAX) min_flag_out |= NEAR_MIN;
  if (max_flag & NO_MAX_RANGE) min_flag_out |= NO_MIN_RANGE;
  return min_flag_out;
}

class RANGE_OPT_PARAM;
/*
  A construction block of the SEL_ARG-graph.

  The following description only covers graphs of SEL_ARG objects with
  sel_arg->type==KEY_RANGE:

  One SEL_ARG object represents an "elementary interval" in form

      min_value <=?  table.keypartX  <=? max_value

  The interval is a non-empty interval of any kind: with[out] minimum/maximum
  bound, [half]open/closed, single-point interval, etc.

  1. SEL_ARG GRAPH STRUCTURE

  SEL_ARG objects are linked together in a graph. The meaning of the graph
  is better demostrated by an example:

     tree->keys[i]
      |
      |             $              $
      |    part=1   $     part=2   $    part=3
      |             $              $
      |  +-------+  $   +-------+  $   +--------+
      |  | kp1<1 |--$-->| kp2=5 |--$-->| kp3=10 |
      |  +-------+  $   +-------+  $   +--------+
      |      |      $              $       |
      |      |      $              $   +--------+
      |      |      $              $   | kp3=12 |
      |      |      $              $   +--------+
      |  +-------+  $              $
      \->| kp1=2 |--$--------------$-+
         +-------+  $              $ |   +--------+
             |      $              $  ==>| kp3=11 |
         +-------+  $              $ |   +--------+
         | kp1=3 |--$--------------$-+       |
         +-------+  $              $     +--------+
             |      $              $     | kp3=14 |
            ...     $              $     +--------+

  The entire graph is partitioned into "interval lists".

  An interval list is a sequence of ordered disjoint intervals over the same
  key part. SEL_ARG are linked via "next" and "prev" pointers. Additionally,
  all intervals in the list form an RB-tree, linked via left/right/parent
  pointers. The RB-tree root SEL_ARG object will be further called "root of the
  interval list".

    In the example pic, there are 4 interval lists:
    "kp<1 OR kp1=2 OR kp1=3", "kp2=5", "kp3=10 OR kp3=12", "kp3=11 OR kp3=13".
    The vertical lines represent SEL_ARG::next/prev pointers.

  In an interval list, each member X may have SEL_ARG::next_key_part pointer
  pointing to the root of another interval list Y. The pointed interval list
  must cover a key part with greater number (i.e. Y->part > X->part).

    In the example pic, the next_key_part pointers are represented by
    horisontal lines.

  2. SEL_ARG GRAPH SEMANTICS

  It represents a condition in a special form (we don't have a name for it ATM)
  The SEL_ARG::next/prev is "OR", and next_key_part is "AND".

  For example, the picture represents the condition in form:
   (kp1 < 1 AND kp2=5 AND (kp3=10 OR kp3=12)) OR
   (kp1=2 AND (kp3=11 OR kp3=14)) OR
   (kp1=3 AND (kp3=11 OR kp3=14))


  3. SEL_ARG GRAPH USE

  Use get_mm_tree() to construct SEL_ARG graph from WHERE condition.
  Then walk the SEL_ARG graph and get a list of dijsoint ordered key
  intervals (i.e. intervals in form

   (constA1, .., const1_K) < (keypart1,.., keypartK) < (constB1, .., constB_K)

  Those intervals can be used to access the index. The uses are in:
   - check_quick_select() - Walk the SEL_ARG graph and find an estimate of
                            how many table records are contained within all
                            intervals.
   - get_quick_select()   - Walk the SEL_ARG, materialize the key intervals,
                            and create QUICK_RANGE_SELECT object that will
                            read records within these intervals.

  4. SPACE COMPLEXITY NOTES

    SEL_ARG graph is a representation of an ordered disjoint sequence of
    intervals over the ordered set of index tuple values.

    For multi-part keys, one can construct a WHERE expression such that its
    list of intervals will be of combinatorial size. Here is an example:

      (keypart1 IN (1,2, ..., n1)) AND
      (keypart2 IN (1,2, ..., n2)) AND
      (keypart3 IN (1,2, ..., n3))

    For this WHERE clause the list of intervals will have n1*n2*n3 intervals
    of form

      (keypart1, keypart2, keypart3) = (k1, k2, k3), where 1 <= k{i} <= n{i}

    SEL_ARG graph structure aims to reduce the amount of required space by
    "sharing" the elementary intervals when possible (the pic at the
    beginning of this comment has examples of such sharing). The sharing may
    prevent combinatorial blowup:

      There are WHERE clauses that have combinatorial-size interval lists but
      will be represented by a compact SEL_ARG graph.
      Example:
        (keypartN IN (1,2, ..., n1)) AND
        ...
        (keypart2 IN (1,2, ..., n2)) AND
        (keypart1 IN (1,2, ..., n3))

    but not in all cases:

    - There are WHERE clauses that do have a compact SEL_ARG-graph
      representation but get_mm_tree() and its callees will construct a
      graph of combinatorial size.
      Example:
        (keypart1 IN (1,2, ..., n1)) AND
        (keypart2 IN (1,2, ..., n2)) AND
        ...
        (keypartN IN (1,2, ..., n3))

    - There are WHERE clauses for which the minimal possible SEL_ARG graph
      representation will have combinatorial size.
      Example:
        By induction: Let's take any interval on some keypart in the middle:

           kp15=c0

        Then let's AND it with this interval 'structure' from preceding and
        following keyparts:

          (kp14=c1 AND kp16=c3) OR keypart14=c2) (*)

        We will obtain this SEL_ARG graph:

             kp14     $      kp15      $      kp16
                      $                $
         +---------+  $   +---------+  $   +---------+
         | kp14=c1 |--$-->| kp15=c0 |--$-->| kp16=c3 |
         +---------+  $   +---------+  $   +---------+
              |       $                $
         +---------+  $   +---------+  $
         | kp14=c2 |--$-->| kp15=c0 |  $
         +---------+  $   +---------+  $
                      $                $

       Note that we had to duplicate "kp15=c0" and there was no way to avoid
       that.
       The induction step: AND the obtained expression with another "wrapping"
       expression like (*).
       When the process ends because of the limit on max. number of keyparts
       we'll have:

         WHERE clause length  is O(3*#max_keyparts)
         SEL_ARG graph size   is O(2^(#max_keyparts/2))

       (it is also possible to construct a case where instead of 2 in 2^n we
        have a bigger constant, e.g. 4, and get a graph with 4^(31/2)= 2^31
        nodes)

    We avoid consuming too much memory by setting a limit on the number of
    SEL_ARG object we can construct during one range analysis invocation.

  5. SEL_ARG GRAPH WEIGHT

    A SEL_ARG graph has a property we call weight, and we define it as follows:

    <definition>
    If the SEL_ARG graph does not have any node with multiple incoming
    next_key_part edges, then its weight is the number of SEL_ARG objects used.

    If there is a node with multiple incoming next_key_part edges, clone that
    node, (and the nodes connected to it via prev/next links) and redirect one
    of the incoming next_key_part edges to the clone.

    Continue with cloning until we get a graph that has no nodes with multiple
    incoming next_key_part edges. Then, the number of SEL_ARG objects in the
    graph is the weight of the original graph.
    </definition>

    Example:

            kp1     $     kp2      $       kp3
                    $              $
      |  +-------+  $              $
      \->| kp1=2 |--$--------------$-+
         +-------+  $              $ |   +--------+
             |      $              $  ==>| kp3=11 |
         +-------+  $              $ |   +--------+
         | kp1>3 |--$--------------$-+       |
         +-------+  $              $     +--------+
                    $              $     | kp3=14 |
                    $              $     +--------+
                    $              $         |
                    $              $     +--------+
                    $              $     | kp3=14 |
                    $              $     +--------+

    Here, the weight is 2 + 2*3=8.

    The rationale behind using this definition of weight is:
    - it has the same order-of-magnitude as the number of ranges that the
      SEL_ARG graph is describing,
    - it is a lot easier to compute than computing the number of ranges,
    - it can be updated incrementally when performing AND/OR operations on
      parts of the graph.

  6. For handling DESC keyparts, See HowRangeOptimizerHandlesDescKeyparts
*/

class SEL_ARG :public Sql_alloc
{
  static int sel_cmp(Field *field, uchar *a, uchar *b, uint8 a_flag,
                     uint8 b_flag);
public:
  uint8 min_flag,max_flag,maybe_flag;
  uint8 part;					// Which key part
  uint8 maybe_null;
  /*
    The ordinal number the least significant component encountered in
    the ranges of the SEL_ARG tree (the first component has number 1)

    Note: this number is currently not precise, it is an upper bound.
    @seealso SEL_ARG::get_max_key_part()
  */
  uint16 max_part_no;
  /*
    Number of children of this element in the RB-tree, plus 1 for this
    element itself.
  */
  uint32 elements;
  /*
    Valid only for elements which are RB-tree roots: Number of times this
    RB-tree is referred to (it is referred by SEL_ARG::next_key_part or by
    SEL_TREE::keys[i] or by a temporary SEL_ARG* variable)
  */
  ulong use_count;

  Field *field;
  uchar *min_value,*max_value;			// Pointer to range

  /*
    eq_tree() requires that left == right == 0 if the type is MAYBE_KEY.
   */
  SEL_ARG *left,*right;   /* R-B tree children */
  SEL_ARG *next,*prev;    /* Links for bi-directional interval list */
  SEL_ARG *parent;        /* R-B tree parent */
  SEL_ARG *next_key_part;
  enum leaf_color { BLACK,RED } color;
  enum Type { IMPOSSIBLE, MAYBE, MAYBE_KEY, KEY_RANGE } type;

  /*
    For R-B root nodes only: the graph weight, as defined above in the
    SEL_ARG GRAPH WEIGHT section.
  */
  uint weight;
  enum { MAX_WEIGHT = 32000 };

#ifndef DBUG_OFF
  uint verify_weight();
#endif

  /* See RANGE_OPT_PARAM::alloced_sel_args */
  enum { MAX_SEL_ARGS = 16000 };

  SEL_ARG() {}
  SEL_ARG(SEL_ARG &);
  SEL_ARG(Field *, const uchar *, const uchar *);
  SEL_ARG(Field *field, uint8 part,
          uchar *min_value, uchar *max_value,
	  uint8 min_flag, uint8 max_flag, uint8 maybe_flag);

  /* This is used to construct degenerate SEL_ARGS like ALWAYS, IMPOSSIBLE, etc */
  SEL_ARG(enum Type type_arg)
    :min_flag(0),
     max_part_no(0) /* first key part means 1. 0 mean 'no parts'*/,
     elements(1),use_count(1),left(0),right(0),
     next_key_part(0), color(BLACK), type(type_arg), weight(1)
  {}
  /**
    returns true if a range predicate is equal. Use all_same()
    to check for equality of all the predicates on this keypart.
  */
  inline bool is_same(const SEL_ARG *arg) const
  {
    if (type != arg->type || part != arg->part)
      return false;
    if (type != KEY_RANGE)
      return true;
    return cmp_min_to_min(arg) == 0 && cmp_max_to_max(arg) == 0;
  }

  uint get_max_key_part() const;

  /**
    returns true if all the predicates in the keypart tree are equal
  */
  bool all_same(const SEL_ARG *arg) const
  {
    if (type != arg->type || part != arg->part)
      return false;
    if (type != KEY_RANGE)
      return true;
    if (arg == this)
      return true;
    const SEL_ARG *cmp_arg= arg->first();
    const SEL_ARG *cur_arg= first();
    for (; cur_arg && cmp_arg && cur_arg->is_same(cmp_arg);
         cur_arg= cur_arg->next, cmp_arg= cmp_arg->next) ;
    if (cur_arg || cmp_arg)
      return false;
    return true;
  }
  inline void merge_flags(SEL_ARG *arg) { maybe_flag|=arg->maybe_flag; }
  inline void maybe_smaller() { maybe_flag=1; }
  /* Return true iff it's a single-point null interval */
  inline bool is_null_interval() { return maybe_null && max_value[0] == 1; }
  inline int cmp_min_to_min(const SEL_ARG* arg) const
  {
    return sel_cmp(field,min_value, arg->min_value, min_flag, arg->min_flag);
  }
  inline int cmp_min_to_max(const SEL_ARG* arg) const
  {
    return sel_cmp(field,min_value, arg->max_value, min_flag, arg->max_flag);
  }
  inline int cmp_max_to_max(const SEL_ARG* arg) const
  {
    return sel_cmp(field,max_value, arg->max_value, max_flag, arg->max_flag);
  }
  inline int cmp_max_to_min(const SEL_ARG* arg) const
  {
    return sel_cmp(field,max_value, arg->min_value, max_flag, arg->min_flag);
  }
  SEL_ARG *clone_and(THD *thd, SEL_ARG* arg)
  {						// Get overlapping range
    uchar *new_min,*new_max;
    uint8 flag_min,flag_max;
    if (cmp_min_to_min(arg) >= 0)
    {
      new_min=min_value; flag_min=min_flag;
    }
    else
    {
      new_min=arg->min_value; flag_min=arg->min_flag; /* purecov: deadcode */
    }
    if (cmp_max_to_max(arg) <= 0)
    {
      new_max=max_value; flag_max=max_flag;
    }
    else
    {
      new_max=arg->max_value; flag_max=arg->max_flag;
    }
    return new (thd->mem_root) SEL_ARG(field, part,
                                       new_min, new_max, flag_min,
                                       flag_max,
                                       MY_TEST(maybe_flag && arg->maybe_flag));
  }
  SEL_ARG *clone_first(SEL_ARG *arg)
  {						// min <= X < arg->min
    return new SEL_ARG(field, part, min_value, arg->min_value,
		       min_flag, arg->min_flag & NEAR_MIN ? 0 : NEAR_MAX,
		       maybe_flag | arg->maybe_flag);
  }
  SEL_ARG *clone_last(SEL_ARG *arg)
  {						// min <= X <= key_max
    return new SEL_ARG(field, part, min_value, arg->max_value,
		       min_flag, arg->max_flag, maybe_flag | arg->maybe_flag);
  }
  SEL_ARG *clone(RANGE_OPT_PARAM *param, SEL_ARG *new_parent, SEL_ARG **next);

  bool copy_min(SEL_ARG* arg)
  {						// Get overlapping range
    if (cmp_min_to_min(arg) > 0)
    {
      min_value=arg->min_value; min_flag=arg->min_flag;
      if ((max_flag & (NO_MAX_RANGE | NO_MIN_RANGE)) ==
	  (NO_MAX_RANGE | NO_MIN_RANGE))
	return 1;				// Full range
    }
    maybe_flag|=arg->maybe_flag;
    return 0;
  }
  bool copy_max(SEL_ARG* arg)
  {						// Get overlapping range
    if (cmp_max_to_max(arg) <= 0)
    {
      max_value=arg->max_value; max_flag=arg->max_flag;
      if ((max_flag & (NO_MAX_RANGE | NO_MIN_RANGE)) ==
	  (NO_MAX_RANGE | NO_MIN_RANGE))
	return 1;				// Full range
    }
    maybe_flag|=arg->maybe_flag;
    return 0;
  }

  void copy_min_to_min(SEL_ARG *arg)
  {
    min_value=arg->min_value; min_flag=arg->min_flag;
  }
  void copy_min_to_max(SEL_ARG *arg)
  {
    max_value=arg->min_value;
    max_flag=arg->min_flag & NEAR_MIN ? 0 : NEAR_MAX;
  }
  void copy_max_to_min(SEL_ARG *arg)
  {
    min_value=arg->max_value;
    min_flag=arg->max_flag & NEAR_MAX ? 0 : NEAR_MIN;
  }
  /* returns a number of keypart values (0 or 1) appended to the key buffer */
  int store_min(uint length, uchar **min_key,uint min_key_flag)
  {
    /* "(kp1 > c1) AND (kp2 OP c2) AND ..." -> (kp1 > c1) */
    if ((min_flag & GEOM_FLAG) ||
        (!(min_flag & NO_MIN_RANGE) &&
	!(min_key_flag & (NO_MIN_RANGE | NEAR_MIN))))
    {
      if (maybe_null && *min_value)
      {
	**min_key=1;
	bzero(*min_key+1,length-1);
      }
      else
	memcpy(*min_key,min_value,length);
      (*min_key)+= length;
      return 1;
    }
    return 0;
  }
  /* returns a number of keypart values (0 or 1) appended to the key buffer */
  int store_max(uint length, uchar **max_key, uint max_key_flag)
  {
    if (!(max_flag & NO_MAX_RANGE) &&
	!(max_key_flag & (NO_MAX_RANGE | NEAR_MAX)))
    {
      if (maybe_null && *max_value)
      {
	**max_key=1;
	bzero(*max_key+1,length-1);
      }
      else
	memcpy(*max_key,max_value,length);
      (*max_key)+= length;
      return 1;
    }
    return 0;
  }

  /* Save minimum and maximum, taking index order into account  */
  void store_min_max(KEY_PART *kp,
                     uint length,
                     uchar **min_key, uint min_flag,
                     uchar **max_key, uint max_flag,
                     int *min_part, int *max_part)
  {
    if (kp[part].flag & HA_REVERSE_SORT) {
      *max_part += store_min(length, max_key, min_flag);
      *min_part += store_max(length, min_key, max_flag);
    } else {
      *min_part += store_min(length, min_key, min_flag);
      *max_part += store_max(length, max_key, max_flag);
    }
  }
  /*
    Get the flag for range's starting endpoint, taking index order into
    account.
  */
  uint get_min_flag(KEY_PART *kp)
  {
    return (kp[part].flag & HA_REVERSE_SORT)? invert_max_flag(max_flag) : min_flag;
  }
  /*
    Get the flag for range's starting endpoint, taking index order into
    account.
  */
  uint get_max_flag(KEY_PART *kp)
  {
    return (kp[part].flag & HA_REVERSE_SORT)? invert_min_flag(min_flag) : max_flag ;
  }
  /* Get the previous interval, taking index order into account */
  inline SEL_ARG* index_order_prev(KEY_PART *kp)
  {
    return (kp[part].flag & HA_REVERSE_SORT)? next : prev;
  }
  /* Get the next interval, taking index order into account */
  inline SEL_ARG* index_order_next(KEY_PART *kp)
  {
    return (kp[part].flag & HA_REVERSE_SORT)? prev : next;
  }

  /*
    Produce a single multi-part interval, taking key part ordering into
    account.
  */
  void store_next_min_max_keys(KEY_PART *key, uchar **cur_min_key,
                               uint *cur_min_flag, uchar **cur_max_key,
                               uint *cur_max_flag, int *min_part,
                               int *max_part);

  /*
    Returns a number of keypart values appended to the key buffer
    for min key and max key. This function is used by both Range
    Analysis and Partition pruning. For partition pruning we have
    to ensure that we don't store also subpartition fields. Thus
    we have to stop at the last partition part and not step into
    the subpartition fields. For Range Analysis we set last_part
    to MAX_KEY which we should never reach.
  */
  int store_min_key(KEY_PART *key,
                    uchar **range_key,
                    uint *range_key_flag,
                    uint last_part,
                    bool start_key)
  {
    SEL_ARG *key_tree= first();
    uint res= key_tree->store_min(key[key_tree->part].store_length,
                                  range_key, *range_key_flag);
    // add flags only if a key_part is written to the buffer
    if (!res)
      return 0;
    *range_key_flag|= key_tree->min_flag;
    SEL_ARG *nkp= key_tree->next_key_part;
    if (nkp && nkp->type == SEL_ARG::KEY_RANGE &&
        key_tree->part != last_part &&
	nkp->part == key_tree->part+1 &&
	!(*range_key_flag & (NO_MIN_RANGE | NEAR_MIN)))
    {
      const bool asc = !(key[key_tree->part].flag & HA_REVERSE_SORT);
      if (start_key == asc)
      {
        res+= nkp->store_min_key(key, range_key, range_key_flag, last_part,
                                 start_key);
      }
      else
      {
        uint tmp_flag = invert_min_flag(*range_key_flag);
        res += nkp->store_max_key(key, range_key, &tmp_flag, last_part,
                                  start_key);
        *range_key_flag = invert_max_flag(tmp_flag);
      }
    }
    return res;
  }

  /* returns a number of keypart values appended to the key buffer */
  int store_max_key(KEY_PART *key,
                    uchar **range_key,
                    uint *range_key_flag,
                    uint last_part,
                    bool start_key)
  {
    SEL_ARG *key_tree= last();
    uint res=key_tree->store_max(key[key_tree->part].store_length,
                                 range_key, *range_key_flag);
    if (!res)
      return 0;
    *range_key_flag|= key_tree->max_flag;
    SEL_ARG *nkp= key_tree->next_key_part;
    if (nkp && nkp->type == SEL_ARG::KEY_RANGE &&
        key_tree->part != last_part &&
	nkp->part == key_tree->part+1 &&
	!(*range_key_flag & (NO_MAX_RANGE | NEAR_MAX)))
    {
      const bool asc = !(key[key_tree->part].flag & HA_REVERSE_SORT);
      if ((!start_key && asc) || (start_key && !asc))
      {
        res += nkp->store_max_key(key, range_key, range_key_flag, last_part,
                                  start_key);
      }
      else
      {
        uint tmp_flag = invert_max_flag(*range_key_flag);
        res += nkp->store_min_key(key, range_key, &tmp_flag, last_part,
                                  start_key);
        *range_key_flag = invert_min_flag(tmp_flag);
      }
    }
    return res;
  }

  SEL_ARG *insert(SEL_ARG *key);
  SEL_ARG *tree_delete(SEL_ARG *key);
  SEL_ARG *find_range(SEL_ARG *key);
  SEL_ARG *rb_insert(SEL_ARG *leaf);
  friend SEL_ARG *rb_delete_fixup(SEL_ARG *root,SEL_ARG *key, SEL_ARG *par);
#ifdef EXTRA_DEBUG
  friend int test_rb_tree(SEL_ARG *element,SEL_ARG *parent);
  void test_use_count(SEL_ARG *root);
#endif
  SEL_ARG *first();
  const SEL_ARG *first() const;
  SEL_ARG *last();
  void make_root();
  inline bool simple_key()
  {
    return !next_key_part && elements == 1;
  }
  void increment_use_count(long count)
  {
    if (next_key_part)
    {
      next_key_part->use_count+=count;
      count*= (next_key_part->use_count-count);
      for (SEL_ARG *pos=next_key_part->first(); pos ; pos=pos->next)
	if (pos->next_key_part)
	  pos->increment_use_count(count);
    }
  }
  void incr_refs()
  {
    increment_use_count(1);
    use_count++;
  }
  void incr_refs_all()
  {
    for (SEL_ARG *pos=first(); pos ; pos=pos->next)
    {
      pos->increment_use_count(1);
    }
    use_count++;
  }
  void free_tree()
  {
    for (SEL_ARG *pos=first(); pos ; pos=pos->next)
      if (pos->next_key_part)
      {
	pos->next_key_part->use_count--;
	pos->next_key_part->free_tree();
      }
  }

  inline SEL_ARG **parent_ptr()
  {
    return parent->left == this ? &parent->left : &parent->right;
  }


  /*
    Check if this SEL_ARG object represents a single-point interval

    SYNOPSIS
      is_singlepoint()

    DESCRIPTION
      Check if this SEL_ARG object (not tree) represents a single-point
      interval, i.e. if it represents a "keypart = const" or
      "keypart IS NULL".

    RETURN
      TRUE   This SEL_ARG object represents a singlepoint interval
      FALSE  Otherwise
  */

  bool is_singlepoint() const
  {
    /*
      Check for NEAR_MIN ("strictly less") and NO_MIN_RANGE (-inf < field)
      flags, and the same for right edge.
    */
    if (min_flag || max_flag)
      return FALSE;
    uchar *min_val= min_value;
    uchar *max_val= max_value;

    if (maybe_null)
    {
      /* First byte is a NULL value indicator */
      if (*min_val != *max_val)
        return FALSE;

      if (*min_val)
        return TRUE; /* This "x IS NULL" */
      min_val++;
      max_val++;
    }
    return !field->key_cmp(min_val, max_val);
  }
  SEL_ARG *clone_tree(RANGE_OPT_PARAM *param);
};

/*
  HowRangeOptimizerHandlesDescKeyparts
  ====================================

  Starting with MySQL-8.0 and MariaDB 10.8, index key parts may be descending,
  for example:

    INDEX idx1(col1, col2 DESC, col3, col4 DESC)

  Range Optimizer handles this as follows:

  Other than that, the SEL_ARG graph is built without any regard to DESC
  keyparts.

  For example, for an index

    INDEX idx2(kp1 DESC, kp2)

  and range

    kp1 BETWEEN 10 and 20       (RANGE-1)

  the SEL_ARG will have min_value=10, max_value=20

  The ordering of key parts is taken into account when SEL_ARG graph is
  linearized to ranges, in sel_arg_range_seq_next() and get_quick_keys().

  The storage engine expects the first bound to be the first in the index and
  the last bound to be the last, that is, for (RANGE-1) we will flip min and
  max and generate these key_range structures:

    start.key='20' , end.key='10'

  See SEL_ARG::store_min_max(). The flag values are flipped as well, see
  SEL_ARG::get_min_flag(), get_max_flag().

  == Handling multiple key parts ==

  For multi-part keys, the order of key parts has an effect on which ranges are
  generated. Consider

    kp1 >= 10 AND kp2 >'foo'

  for INDEX(kp1 ASC, kp2 ASC) the range will be

    (kp1, kp2) > (10, 'foo')

  while for INDEX(kp1 ASC, kp2 DESC) it will be just

    kp1 >= 10

  Another example:

    (kp1 BETWEEN 10 AND 20) AND (kp2 BETWEEN 'foo' AND 'quux')

  with INDEX (kp1 ASC, kp2 ASC) will generate

    (10, 'foo') <= (kp1, kp2) < (20, 'quux')

  while with index INDEX (kp1 ASC, kp2 DESC) it will generate

    (10, 'quux') <= (kp1, kp2) < (20, 'foo')

  This is again achieved by sel_arg_range_seq_next() and get_quick_keys()
  flipping SEL_ARG's min,max, their flags and next/prev as needed.
*/

extern MYSQL_PLUGIN_IMPORT SEL_ARG null_element;

class SEL_ARG_IMPOSSIBLE: public SEL_ARG
{
public:
  SEL_ARG_IMPOSSIBLE(Field *field)
   :SEL_ARG(field, 0, 0)
  {
    type= SEL_ARG::IMPOSSIBLE;
  }
};


class RANGE_OPT_PARAM
{
public:
  THD	*thd;   /* Current thread handle */
  TABLE *table; /* Table being analyzed */
  table_map prev_tables;
  table_map read_tables;
  table_map current_table; /* Bit of the table being analyzed */

  /* Array of parts of all keys for which range analysis is performed */
  KEY_PART *key_parts;
  KEY_PART *key_parts_end;
  MEM_ROOT *mem_root; /* Memory that will be freed when range analysis completes */
  MEM_ROOT *old_root; /* Memory that will last until the query end */
  /*
    Number of indexes used in range analysis (In SEL_TREE::keys only first
    #keys elements are not empty)
  */
  uint keys;

  /*
    If true, the index descriptions describe real indexes (and it is ok to
    call field->optimize_range(real_keynr[...], ...).
    Otherwise index description describes fake indexes.
  */
  bool using_real_indexes;

  /*
    Aggressively remove "scans" that do not have conditions on first
    keyparts. Such scans are usable when doing partition pruning but not
    regular range optimization.
  */
  bool remove_jump_scans;

  /*
    TRUE <=> Range analyzer should remove parts of condition that are found
    to be always FALSE.
  */
  bool remove_false_where_parts;

  /*
    used_key_no -> table_key_no translation table. Only makes sense if
    using_real_indexes==TRUE
  */
  uint real_keynr[MAX_KEY];

  /*
    Used to store 'current key tuples', in both range analysis and
    partitioning (list) analysis
  */
  uchar *min_key;
  uchar *max_key;

  /* Number of SEL_ARG objects allocated by SEL_ARG::clone_tree operations */
  uint alloced_sel_args;

  bool force_default_mrr;
  KEY_PART *key[MAX_KEY]; /* First key parts of keys used in the query */

  bool statement_should_be_aborted() const
  {
    return
      thd->killed ||
      thd->is_fatal_error ||
      thd->is_error() ||
      alloced_sel_args > SEL_ARG::MAX_SEL_ARGS;
  }
};


class Explain_quick_select;
/*
  A "MIN_TUPLE < tbl.key_tuple < MAX_TUPLE" interval. 
  
  One of endpoints may be absent. 'flags' member has flags which tell whether
  the endpoints are '<' or '<='.
*/
class QUICK_RANGE :public Sql_alloc {
 public:
  uchar *min_key,*max_key;
  uint16 min_length,max_length,flag;
  key_part_map min_keypart_map, // bitmap of used keyparts in min_key
               max_keypart_map; // bitmap of used keyparts in max_key
#ifdef HAVE_valgrind
  uint16 dummy;					/* Avoid warnings on 'flag' */
#endif
  QUICK_RANGE();				/* Full range */
  QUICK_RANGE(THD *thd, const uchar *min_key_arg, uint min_length_arg,
              key_part_map min_keypart_map_arg,
	      const uchar *max_key_arg, uint max_length_arg,
              key_part_map max_keypart_map_arg,
	      uint flag_arg)
    : min_key((uchar*) thd->memdup(min_key_arg, min_length_arg + 1)),
      max_key((uchar*) thd->memdup(max_key_arg, max_length_arg + 1)),
      min_length((uint16) min_length_arg),
      max_length((uint16) max_length_arg),
      flag((uint16) flag_arg),
      min_keypart_map(min_keypart_map_arg),
      max_keypart_map(max_keypart_map_arg)
    {
#ifdef HAVE_valgrind
      dummy=0;
#endif
    }

  /**
     Initializes a key_range object for communication with storage engine. 

     This function facilitates communication with the Storage Engine API by
     translating the minimum endpoint of the interval represented by this
     QUICK_RANGE into an index range endpoint specifier for the engine.

     @param Pointer to an uninitialized key_range C struct.

     @param prefix_length The length of the search key prefix to be used for
     lookup.
     
     @param keypart_map A set (bitmap) of keyparts to be used.
  */
  void make_min_endpoint(key_range *kr, uint prefix_length, 
                         key_part_map keypart_map) {
    make_min_endpoint(kr);
    kr->length= MY_MIN(kr->length, prefix_length);
    kr->keypart_map&= keypart_map;
  }
  
  /**
     Initializes a key_range object for communication with storage engine. 

     This function facilitates communication with the Storage Engine API by
     translating the minimum endpoint of the interval represented by this
     QUICK_RANGE into an index range endpoint specifier for the engine.

     @param Pointer to an uninitialized key_range C struct.
  */
  void make_min_endpoint(key_range *kr) {
    kr->key= (const uchar*)min_key;
    kr->length= min_length;
    kr->keypart_map= min_keypart_map;
    kr->flag= ((flag & NEAR_MIN) ? HA_READ_AFTER_KEY :
               (flag & EQ_RANGE) ? HA_READ_KEY_EXACT : HA_READ_KEY_OR_NEXT);
  }

  /**
     Initializes a key_range object for communication with storage engine. 

     This function facilitates communication with the Storage Engine API by
     translating the maximum endpoint of the interval represented by this
     QUICK_RANGE into an index range endpoint specifier for the engine.

     @param Pointer to an uninitialized key_range C struct.

     @param prefix_length The length of the search key prefix to be used for
     lookup.
     
     @param keypart_map A set (bitmap) of keyparts to be used.
  */
  void make_max_endpoint(key_range *kr, uint prefix_length, 
                         key_part_map keypart_map) {
    make_max_endpoint(kr);
    kr->length= MY_MIN(kr->length, prefix_length);
    kr->keypart_map&= keypart_map;
  }

  /**
     Initializes a key_range object for communication with storage engine. 

     This function facilitates communication with the Storage Engine API by
     translating the maximum endpoint of the interval represented by this
     QUICK_RANGE into an index range endpoint specifier for the engine.

     @param Pointer to an uninitialized key_range C struct.
  */
  void make_max_endpoint(key_range *kr) {
    kr->key= (const uchar*)max_key;
    kr->length= max_length;
    kr->keypart_map= max_keypart_map;
    /*
      We use READ_AFTER_KEY here because if we are reading on a key
      prefix we want to find all keys with this prefix
    */
    kr->flag= (flag & NEAR_MAX ? HA_READ_BEFORE_KEY : HA_READ_AFTER_KEY);
  }
};


/*
  Quick select interface.
  This class is a parent for all QUICK_*_SELECT and FT_SELECT classes.

  The usage scenario is as follows:
  1. Create quick select
    quick= new QUICK_XXX_SELECT(...);

  2. Perform lightweight initialization. This can be done in 2 ways:
  2.a: Regular initialization
    if (quick->init())
    {
      //the only valid action after failed init() call is delete
      delete quick;
    }
  2.b: Special initialization for quick selects merged by QUICK_ROR_*_SELECT
    if (quick->init_ror_merged_scan())
      delete quick;

  3. Perform zero, one, or more scans.
    while (...)
    {
      // initialize quick select for scan. This may allocate
      // buffers and/or prefetch rows.
      if (quick->reset())
      {
        //the only valid action after failed reset() call is delete
        delete quick;
        //abort query
      }

      // perform the scan
      do
      {
        res= quick->get_next();
      } while (res && ...)
    }

  4. Delete the select:
    delete quick;
  
  NOTE 
    quick select doesn't use Sql_alloc/MEM_ROOT allocation because "range
    checked for each record" functionality may create/destroy
    O(#records_in_some_table) quick selects during query execution.
*/

class QUICK_SELECT_I
{
public:
  ha_rows records;  /* estimate of # of records to be retrieved */
  double  read_time; /* time to perform this retrieval          */
  TABLE   *head;
  /*
    Index this quick select uses, or MAX_KEY for quick selects
    that use several indexes
  */
  uint index;

  /*
    Total length of first used_key_parts parts of the key.
    Applicable if index!= MAX_KEY.
  */
  uint max_used_key_length;

  /*
    Max. number of (first) key parts this quick select uses for retrieval.
    eg. for "(key1p1=c1 AND key1p2=c2) OR key1p1=c2" used_key_parts == 2.
    Applicable if index!= MAX_KEY.

    For QUICK_GROUP_MIN_MAX_SELECT it includes MIN/MAX argument keyparts.
  */
  uint used_key_parts;

  QUICK_SELECT_I();
  virtual ~QUICK_SELECT_I(){};

  /*
    Do post-constructor initialization.
    SYNOPSIS
      init()

    init() performs initializations that should have been in constructor if
    it was possible to return errors from constructors. The join optimizer may
    create and then delete quick selects without retrieving any rows so init()
    must not contain any IO or CPU intensive code.

    If init() call fails the only valid action is to delete this quick select,
    reset() and get_next() must not be called.

    RETURN
      0      OK
      other  Error code
  */
  virtual int  init() = 0;

  /*
    Initialize quick select for row retrieval.
    SYNOPSIS
      reset()

    reset() should be called when it is certain that row retrieval will be
    necessary. This call may do heavyweight initialization like buffering first
    N records etc. If reset() call fails get_next() must not be called.
    Note that reset() may be called several times if 
     * the quick select is executed in a subselect
     * a JOIN buffer is used
    
    RETURN
      0      OK
      other  Error code
  */
  virtual int  reset(void) = 0;

  virtual int  get_next() = 0;   /* get next record to retrieve */

  /* Range end should be called when we have looped over the whole index */
  virtual void range_end() {}

  virtual bool reverse_sorted() = 0;
  virtual bool unique_key_range() { return false; }

  /*
    Request that this quick select produces sorted output. Not all quick
    selects can do it, the caller is responsible for calling this function
    only for those quick selects that can.
  */
  virtual void need_sorted_output() = 0;
  enum {
    QS_TYPE_RANGE = 0,
    QS_TYPE_INDEX_INTERSECT = 1,
    QS_TYPE_INDEX_MERGE = 2,
    QS_TYPE_RANGE_DESC = 3,
    QS_TYPE_FULLTEXT   = 4,
    QS_TYPE_ROR_INTERSECT = 5,
    QS_TYPE_ROR_UNION = 6,
    QS_TYPE_GROUP_MIN_MAX = 7
  };

  /* Get type of this quick select - one of the QS_TYPE_* values */
  virtual int get_type() = 0;

  /*
    Initialize this quick select as a merged scan inside a ROR-union or a ROR-
    intersection scan. The caller must not additionally call init() if this
    function is called.
    SYNOPSIS
      init_ror_merged_scan()
        reuse_handler  If true, the quick select may use table->handler,
                       otherwise it must create and use a separate handler
                       object.
    RETURN
      0     Ok
      other Error
  */
  virtual int init_ror_merged_scan(bool reuse_handler, MEM_ROOT *alloc)
  { DBUG_ASSERT(0); return 1; }

  /*
    Save ROWID of last retrieved row in file->ref. This used in ROR-merging.
  */
  virtual void save_last_pos(){};
  
  void add_key_and_length(String *key_names,
                          String *used_lengths,
                          bool *first);

  /*
    Append comma-separated list of keys this quick select uses to key_names;
    append comma-separated list of corresponding used lengths to used_lengths.
    This is used by select_describe.
  */
  virtual void add_keys_and_lengths(String *key_names,
                                    String *used_lengths)=0;

  void add_key_name(String *str, bool *first);

  /* Save information about quick select's query plan */
  virtual Explain_quick_select* get_explain(MEM_ROOT *alloc)= 0;

  /*
    Return 1 if any index used by this quick select
    uses field which is marked in passed bitmap.
  */
  virtual bool is_keys_used(const MY_BITMAP *fields);

  /**
    Simple sanity check that the quick select has been set up
    correctly. Function is overridden by quick selects that merge
    indices.
   */
  virtual bool is_valid() { return index != MAX_KEY; };

  /*
    rowid of last row retrieved by this quick select. This is used only when
    doing ROR-index_merge selects
  */
  uchar    *last_rowid;

  /*
    Table record buffer used by this quick select.
  */
  uchar    *record;

  virtual void replace_handler(handler *new_file)
  {
    DBUG_ASSERT(0); /* Only supported in QUICK_RANGE_SELECT */
  }

#ifndef DBUG_OFF
  /*
    Print quick select information to DBUG_FILE. Caller is responsible
    for locking DBUG_FILE before this call and unlocking it afterwards.
  */
  virtual void dbug_dump(int indent, bool verbose)= 0;
#endif

  /*
    Returns a QUICK_SELECT with reverse order of to the index.
  */
  virtual QUICK_SELECT_I *make_reverse(uint used_key_parts_arg) { return NULL; }

  /*
    Add the key columns used by the quick select into table's read set.

    This is used by an optimization in filesort.
  */
  virtual void add_used_key_part_to_set()=0;
};


struct st_qsel_param;
class PARAM;


/*
  MRR range sequence, array<QUICK_RANGE> implementation: sequence traversal
  context.
*/
typedef struct st_quick_range_seq_ctx
{
  QUICK_RANGE **first;
  QUICK_RANGE **cur;
  QUICK_RANGE **last;
} QUICK_RANGE_SEQ_CTX;

range_seq_t quick_range_seq_init(void *init_param, uint n_ranges, uint flags);
bool quick_range_seq_next(range_seq_t rseq, KEY_MULTI_RANGE *range);


/*
  Quick select that does a range scan on a single key. The records are
  returned in key order.
*/
class QUICK_RANGE_SELECT : public QUICK_SELECT_I
{
protected:
  THD *thd;
  bool no_alloc;
  MEM_ROOT *parent_alloc;

  /* true if we enabled key only reads */
  handler *file;

  /* Members to deal with case when this quick select is a ROR-merged scan */
  bool in_ror_merged_scan;
  MY_BITMAP column_bitmap;
  bool free_file;   /* TRUE <=> this->file is "owned" by this quick select */

  /* Range pointers to be used when not using MRR interface */
  /* Members needed to use the MRR interface */
  QUICK_RANGE_SEQ_CTX qr_traversal_ctx;
public:
  uint mrr_flags; /* Flags to be used with MRR interface */
protected:
  uint mrr_buf_size; /* copy from thd->variables.mrr_buff_size */  
  HANDLER_BUFFER *mrr_buf_desc; /* the handler buffer */

  /* Info about index we're scanning */
  
  DYNAMIC_ARRAY ranges;     /* ordered array of range ptrs */
  QUICK_RANGE **cur_range;  /* current element in ranges  */
  
  QUICK_RANGE *last_range;
  
  KEY_PART *key_parts;
  KEY_PART_INFO *key_part_info;
  
  bool dont_free; /* Used by QUICK_SELECT_DESC */

  int cmp_next(QUICK_RANGE *range);
  int cmp_prev(QUICK_RANGE *range);
  bool row_in_ranges();
public:
  MEM_ROOT alloc;

  QUICK_RANGE_SELECT(THD *thd, TABLE *table,uint index_arg,bool no_alloc,
                     MEM_ROOT *parent_alloc, bool *create_err);
  ~QUICK_RANGE_SELECT();
  virtual QUICK_RANGE_SELECT *clone(bool *create_error)
    { return new QUICK_RANGE_SELECT(thd, head, index, no_alloc, parent_alloc,
                                    create_error); }
  
  void need_sorted_output();
  int init();
  int reset(void);
  int get_next();
  void range_end();
  int get_next_prefix(uint prefix_length, uint group_key_parts, 
                      uchar *cur_prefix);
  bool reverse_sorted() { return 0; }
  bool unique_key_range();
  int init_ror_merged_scan(bool reuse_handler, MEM_ROOT *alloc);
  void save_last_pos()
  { file->position(record); }
  int get_type() { return QS_TYPE_RANGE; }
  void add_keys_and_lengths(String *key_names, String *used_lengths);
  Explain_quick_select *get_explain(MEM_ROOT *alloc);
#ifndef DBUG_OFF
  void dbug_dump(int indent, bool verbose);
#endif
  virtual void replace_handler(handler *new_file) { file= new_file; }
  QUICK_SELECT_I *make_reverse(uint used_key_parts_arg);

  virtual void add_used_key_part_to_set();

private:
  /* Default copy ctor used by QUICK_SELECT_DESC */
  friend class TRP_ROR_INTERSECT;
  friend
  QUICK_RANGE_SELECT *get_quick_select_for_ref(THD *thd, TABLE *table,
                                               struct st_table_ref *ref,
                                               ha_rows records);
  friend bool get_quick_keys(PARAM *param, QUICK_RANGE_SELECT *quick, 
                             KEY_PART *key, SEL_ARG *key_tree, 
                             uchar *min_key, uint min_key_flag,
                             uchar *max_key, uint max_key_flag);
  friend QUICK_RANGE_SELECT *get_quick_select(PARAM*,uint idx,
                                              SEL_ARG *key_tree,
                                              uint mrr_flags,
                                              uint mrr_buf_size,
                                              MEM_ROOT *alloc);
  friend class QUICK_SELECT_DESC;
  friend class QUICK_INDEX_SORT_SELECT;
  friend class QUICK_INDEX_MERGE_SELECT;
  friend class QUICK_ROR_INTERSECT_SELECT;
  friend class QUICK_INDEX_INTERSECT_SELECT;
  friend class QUICK_GROUP_MIN_MAX_SELECT;
  friend bool quick_range_seq_next(range_seq_t rseq, KEY_MULTI_RANGE *range);
  friend range_seq_t quick_range_seq_init(void *init_param,
                                          uint n_ranges, uint flags);
  friend 
  int read_keys_and_merge_scans(THD *thd, TABLE *head,
                                List<QUICK_RANGE_SELECT> quick_selects,
                                QUICK_RANGE_SELECT *pk_quick_select,
                                READ_RECORD *read_record,
                                bool intersection,
                                key_map *filtered_scans,
                                Unique **unique_ptr);

};


class QUICK_RANGE_SELECT_GEOM: public QUICK_RANGE_SELECT
{
public:
  QUICK_RANGE_SELECT_GEOM(THD *thd, TABLE *table, uint index_arg,
                          bool no_alloc, MEM_ROOT *parent_alloc, 
                          bool *create_err)
    :QUICK_RANGE_SELECT(thd, table, index_arg, no_alloc, parent_alloc,
    create_err)
    {};
  virtual QUICK_RANGE_SELECT *clone(bool *create_error)
    {
      DBUG_ASSERT(0);
      return new QUICK_RANGE_SELECT_GEOM(thd, head, index, no_alloc,
                                         parent_alloc, create_error);
    }
  virtual int get_next();
};


/*
  QUICK_INDEX_SORT_SELECT is the base class for the common functionality of:
  - QUICK_INDEX_MERGE_SELECT, access based on multi-index merge/union 
  - QUICK_INDEX_INTERSECT_SELECT, access based on  multi-index intersection 
    

    QUICK_INDEX_SORT_SELECT uses
     * QUICK_RANGE_SELECTs to get rows
     * Unique class
       - to remove duplicate rows for QUICK_INDEX_MERGE_SELECT
       - to intersect rows for QUICK_INDEX_INTERSECT_SELECT

  INDEX MERGE OPTIMIZER
    Current implementation doesn't detect all cases where index merge could
    be used, in particular:

     * index_merge+'using index' is not supported

     * If WHERE part contains complex nested AND and OR conditions, some ways
       to retrieve rows using index merge will not be considered. The choice
       of read plan may depend on the order of conjuncts/disjuncts in WHERE
       part of the query, see comments near imerge_list_or_list and
       SEL_IMERGE::or_sel_tree_with_checks functions for details.

     * There is no "index_merge_ref" method (but index merge on non-first
       table in join is possible with 'range checked for each record').


  ROW RETRIEVAL ALGORITHM

    index merge/intersection uses Unique class for duplicates removal. 
    index merge/intersection takes advantage of Clustered Primary Key (CPK)
    if the table has one.
    The index merge/intersection algorithm consists of two phases:

    Phase 1 
    (implemented by a QUICK_INDEX_MERGE_SELECT::read_keys_and_merge call):

    prepare()
    {
      activate 'index only';
      while(retrieve next row for non-CPK scan)
      {
        if (there is a CPK scan and row will be retrieved by it)
          skip this row;
        else
          put its rowid into Unique;
      }
      deactivate 'index only';
    }

    Phase 2 
    (implemented as sequence of QUICK_INDEX_MERGE_SELECT::get_next calls):

    fetch()
    {
      retrieve all rows from row pointers stored in Unique
      (merging/intersecting them);
      free Unique;
      if (! intersection) 
        retrieve all rows for CPK scan;
    }
*/

class QUICK_INDEX_SORT_SELECT : public QUICK_SELECT_I
{
protected:
  Unique *unique;
public:
  QUICK_INDEX_SORT_SELECT(THD *thd, TABLE *table);
  ~QUICK_INDEX_SORT_SELECT();

  int  init();
  void need_sorted_output() { DBUG_ASSERT(0); /* Can't do it */ }
  int  reset(void);
  bool reverse_sorted() { return false; }
  bool unique_key_range() { return false; }
  bool is_keys_used(const MY_BITMAP *fields);
#ifndef DBUG_OFF
  void dbug_dump(int indent, bool verbose);
#endif
  Explain_quick_select *get_explain(MEM_ROOT *alloc);

  bool push_quick_back(QUICK_RANGE_SELECT *quick_sel_range);

  /* range quick selects this index merge/intersect consists of */
  List<QUICK_RANGE_SELECT> quick_selects;

  /* quick select that uses clustered primary key (NULL if none) */
  QUICK_RANGE_SELECT* pk_quick_select;

  MEM_ROOT alloc;
  THD *thd;
  virtual bool is_valid()
  {
    List_iterator_fast<QUICK_RANGE_SELECT> it(quick_selects);
    QUICK_RANGE_SELECT *quick;
    bool valid= true;
    while ((quick= it++))
    {
      if (!quick->is_valid())
      {
        valid= false;
        break;
      }
    }
    return valid;
  }
  virtual int read_keys_and_merge()= 0;
  /* used to get rows collected in Unique */
  READ_RECORD read_record;

  virtual void add_used_key_part_to_set();
};



class QUICK_INDEX_MERGE_SELECT : public QUICK_INDEX_SORT_SELECT
{
private:
  /* true if this select is currently doing a clustered PK scan */
  bool  doing_pk_scan;
protected:
  int read_keys_and_merge();

public:
  QUICK_INDEX_MERGE_SELECT(THD *thd_arg, TABLE *table)
    :QUICK_INDEX_SORT_SELECT(thd_arg, table) {}

  int get_next();
  int get_type() { return QS_TYPE_INDEX_MERGE; }
  void add_keys_and_lengths(String *key_names, String *used_lengths);
};

class QUICK_INDEX_INTERSECT_SELECT : public QUICK_INDEX_SORT_SELECT
{
protected:
  int read_keys_and_merge();

public:
  QUICK_INDEX_INTERSECT_SELECT(THD *thd_arg, TABLE *table)
    :QUICK_INDEX_SORT_SELECT(thd_arg, table) {}

  key_map filtered_scans;
  int get_next();
  int get_type() { return QS_TYPE_INDEX_INTERSECT; }
  void add_keys_and_lengths(String *key_names, String *used_lengths);
  Explain_quick_select *get_explain(MEM_ROOT *alloc);
};


/*
  Rowid-Ordered Retrieval (ROR) index intersection quick select.
  This quick select produces intersection of row sequences returned
  by several QUICK_RANGE_SELECTs it "merges".

  All merged QUICK_RANGE_SELECTs must return rowids in rowid order.
  QUICK_ROR_INTERSECT_SELECT will return rows in rowid order, too.

  All merged quick selects retrieve {rowid, covered_fields} tuples (not full
  table records).
  QUICK_ROR_INTERSECT_SELECT retrieves full records if it is not being used
  by QUICK_ROR_INTERSECT_SELECT and all merged quick selects together don't
  cover needed all fields.

  If one of the merged quick selects is a Clustered PK range scan, it is
  used only to filter rowid sequence produced by other merged quick selects.
*/

class QUICK_ROR_INTERSECT_SELECT : public QUICK_SELECT_I
{
public:
  QUICK_ROR_INTERSECT_SELECT(THD *thd, TABLE *table,
                             bool retrieve_full_rows,
                             MEM_ROOT *parent_alloc);
  ~QUICK_ROR_INTERSECT_SELECT();

  int  init();
  void need_sorted_output() { DBUG_ASSERT(0); /* Can't do it */ }
  int  reset(void);
  int  get_next();
  bool reverse_sorted() { return false; }
  bool unique_key_range() { return false; }
  int get_type() { return QS_TYPE_ROR_INTERSECT; }
  void add_keys_and_lengths(String *key_names, String *used_lengths);
  Explain_quick_select *get_explain(MEM_ROOT *alloc);
  bool is_keys_used(const MY_BITMAP *fields);
  void add_used_key_part_to_set();
#ifndef DBUG_OFF
  void dbug_dump(int indent, bool verbose);
#endif
  int init_ror_merged_scan(bool reuse_handler, MEM_ROOT *alloc);
  bool push_quick_back(MEM_ROOT *alloc, QUICK_RANGE_SELECT *quick_sel_range);

  class QUICK_SELECT_WITH_RECORD : public Sql_alloc
  {
  public:
    QUICK_RANGE_SELECT *quick;
    uchar *key_tuple;
    ~QUICK_SELECT_WITH_RECORD() { delete quick; }
  };

  /*
    Range quick selects this intersection consists of, not including
    cpk_quick.
  */
  List<QUICK_SELECT_WITH_RECORD> quick_selects;

  virtual bool is_valid()
  {
    List_iterator_fast<QUICK_SELECT_WITH_RECORD> it(quick_selects);
    QUICK_SELECT_WITH_RECORD *quick;
    bool valid= true;
    while ((quick= it++))
    {
      if (!quick->quick->is_valid())
      {
        valid= false;
        break;
      }
    }
    return valid;
  }

  /*
    Merged quick select that uses Clustered PK, if there is one. This quick
    select is not used for row retrieval, it is used for row retrieval.
  */
  QUICK_RANGE_SELECT *cpk_quick;

  MEM_ROOT alloc; /* Memory pool for this and merged quick selects data. */
  THD *thd;       /* current thread */
  bool need_to_fetch_row; /* if true, do retrieve full table records. */
  /* in top-level quick select, true if merged scans where initialized */
  bool scans_inited; 
};


/*
  Rowid-Ordered Retrieval index union select.
  This quick select produces union of row sequences returned by several
  quick select it "merges".

  All merged quick selects must return rowids in rowid order.
  QUICK_ROR_UNION_SELECT will return rows in rowid order, too.

  All merged quick selects are set not to retrieve full table records.
  ROR-union quick select always retrieves full records.

*/

class QUICK_ROR_UNION_SELECT : public QUICK_SELECT_I
{
public:
  QUICK_ROR_UNION_SELECT(THD *thd, TABLE *table);
  ~QUICK_ROR_UNION_SELECT();

  int  init();
  void need_sorted_output() { DBUG_ASSERT(0); /* Can't do it */ }
  int  reset(void);
  int  get_next();
  bool reverse_sorted() { return false; }
  bool unique_key_range() { return false; }
  int get_type() { return QS_TYPE_ROR_UNION; }
  void add_keys_and_lengths(String *key_names, String *used_lengths);
  Explain_quick_select *get_explain(MEM_ROOT *alloc);
  bool is_keys_used(const MY_BITMAP *fields);
  void add_used_key_part_to_set();
#ifndef DBUG_OFF
  void dbug_dump(int indent, bool verbose);
#endif

  bool push_quick_back(QUICK_SELECT_I *quick_sel_range);

  List<QUICK_SELECT_I> quick_selects; /* Merged quick selects */

  virtual bool is_valid()
  {
    List_iterator_fast<QUICK_SELECT_I> it(quick_selects);
    QUICK_SELECT_I *quick;
    bool valid= true;
    while ((quick= it++))
    {
      if (!quick->is_valid())
      {
        valid= false;
        break;
      }
    }
    return valid;
  }

  QUEUE queue;    /* Priority queue for merge operation */
  MEM_ROOT alloc; /* Memory pool for this and merged quick selects data. */

  THD *thd;             /* current thread */
  uchar *cur_rowid;      /* buffer used in get_next() */
  uchar *prev_rowid;     /* rowid of last row returned by get_next() */
  bool have_prev_rowid; /* true if prev_rowid has valid data */
  uint rowid_length;    /* table rowid length */
private:
  bool scans_inited; 
};


/*
  Index scan for GROUP-BY queries with MIN/MAX aggregate functions.

  This class provides a specialized index access method for GROUP-BY queries
  of the forms:

       SELECT A_1,...,A_k, [B_1,...,B_m], [MIN(C)], [MAX(C)]
         FROM T
        WHERE [RNG(A_1,...,A_p ; where p <= k)]
         [AND EQ(B_1,...,B_m)]
         [AND PC(C)]
         [AND PA(A_i1,...,A_iq)]
       GROUP BY A_1,...,A_k;

    or

       SELECT DISTINCT A_i1,...,A_ik
         FROM T
        WHERE [RNG(A_1,...,A_p ; where p <= k)]
         [AND PA(A_i1,...,A_iq)];

  where all selected fields are parts of the same index.
  The class of queries that can be processed by this quick select is fully
  specified in the description of get_best_trp_group_min_max() in opt_range.cc.

  The get_next() method directly produces result tuples, thus obviating the
  need to call end_send_group() because all grouping is already done inside
  get_next().

  Since one of the requirements is that all select fields are part of the same
  index, this class produces only index keys, and not complete records.
*/

class QUICK_GROUP_MIN_MAX_SELECT : public QUICK_SELECT_I
{
private:
  handler * const file;   /* The handler used to get data. */
  JOIN *join;            /* Descriptor of the current query */
  KEY  *index_info;      /* The index chosen for data access */
  uchar *record;          /* Buffer where the next record is returned. */
  uchar *tmp_record;      /* Temporary storage for next_min(), next_max(). */
  uchar *group_prefix;    /* Key prefix consisting of the GROUP fields. */
  const uint group_prefix_len; /* Length of the group prefix. */
  uint group_key_parts;  /* A number of keyparts in the group prefix */
  uchar *last_prefix;     /* Prefix of the last group for detecting EOF. */
  bool have_min;         /* Specify whether we are computing */
  bool have_max;         /*   a MIN, a MAX, or both.         */
  bool have_agg_distinct;/*   aggregate_function(DISTINCT ...).  */
  bool seen_first_key;   /* Denotes whether the first key was retrieved.*/
  bool doing_key_read;   /* true if we enabled key only reads */

  KEY_PART_INFO *min_max_arg_part; /* The keypart of the only argument field */
                                   /* of all MIN/MAX functions.              */
  uint min_max_arg_len;  /* The length of the MIN/MAX argument field */
  uchar *key_infix;       /* Infix of constants from equality predicates. */
  uint key_infix_len;
  DYNAMIC_ARRAY min_max_ranges; /* Array of range ptrs for the MIN/MAX field. */
  uint real_prefix_len; /* Length of key prefix extended with key_infix. */
  uint real_key_parts;  /* A number of keyparts in the above value.      */
  List<Item_sum> *min_functions;
  List<Item_sum> *max_functions;
  List_iterator<Item_sum> *min_functions_it;
  List_iterator<Item_sum> *max_functions_it;
  /* 
    Use index scan to get the next different key instead of jumping into it 
    through index read 
  */
  bool is_index_scan; 
public:
  /*
    The following two members are public to allow easy access from
    TRP_GROUP_MIN_MAX::make_quick()
  */
  MEM_ROOT alloc; /* Memory pool for this and quick_prefix_select data. */
  QUICK_RANGE_SELECT *quick_prefix_select;/* For retrieval of group prefixes. */
private:
  int  next_prefix();
  int  next_min_in_range();
  int  next_max_in_range();
  int  next_min();
  int  next_max();
  void update_min_result();
  void update_max_result();
  int cmp_min_max_key(const uchar *key, uint16 length);
public:
  QUICK_GROUP_MIN_MAX_SELECT(TABLE *table, JOIN *join, bool have_min,
                             bool have_max, bool have_agg_distinct,
                             KEY_PART_INFO *min_max_arg_part,
                             uint group_prefix_len, uint group_key_parts,
                             uint used_key_parts, KEY *index_info, uint
                             use_index, double read_cost, ha_rows records, uint
                             key_infix_len, uchar *key_infix, MEM_ROOT
                             *parent_alloc, bool is_index_scan);
  ~QUICK_GROUP_MIN_MAX_SELECT();
  bool add_range(SEL_ARG *sel_range);
  void update_key_stat();
  void adjust_prefix_ranges();
  bool alloc_buffers();
  int init();
  void need_sorted_output() { /* always do it */ }
  int reset();
  int get_next();
  bool reverse_sorted() { return false; }
  bool unique_key_range() { return false; }
  int get_type() { return QS_TYPE_GROUP_MIN_MAX; }
  void add_keys_and_lengths(String *key_names, String *used_lengths);
  void add_used_key_part_to_set();
#ifndef DBUG_OFF
  void dbug_dump(int indent, bool verbose);
#endif
  bool is_agg_distinct() { return have_agg_distinct; }
  bool loose_scan_is_scanning() { return is_index_scan; }
  Explain_quick_select *get_explain(MEM_ROOT *alloc);
};


class QUICK_SELECT_DESC: public QUICK_RANGE_SELECT
{
public:
  QUICK_SELECT_DESC(QUICK_RANGE_SELECT *q, uint used_key_parts);
  virtual QUICK_RANGE_SELECT *clone(bool *create_error)
    { DBUG_ASSERT(0); return new QUICK_SELECT_DESC(this, used_key_parts); }
  int get_next();
  bool reverse_sorted() { return 1; }
  int get_type() { return QS_TYPE_RANGE_DESC; }
  QUICK_SELECT_I *make_reverse(uint used_key_parts_arg)
  {
    return this; // is already reverse sorted
  }
private:
  bool range_reads_after_key(QUICK_RANGE *range);
  int reset(void) { rev_it.rewind(); return QUICK_RANGE_SELECT::reset(); }
  List<QUICK_RANGE> rev_ranges;
  List_iterator<QUICK_RANGE> rev_it;
  uint used_key_parts;
};


class SQL_SELECT :public Sql_alloc {
 public:
  QUICK_SELECT_I *quick;	// If quick-select used
  COND		*cond;		// where condition

  /*
    When using Index Condition Pushdown: condition that we've had before
    extracting and pushing index condition.
    In other cases, NULL.
  */
  Item *pre_idx_push_select_cond;
  TABLE	*head;
  IO_CACHE file;		// Positions to used records
  ha_rows records;		// Records in use if read from file
  double read_time;		// Time to read rows
  key_map quick_keys;		// Possible quick keys
  key_map needed_reg;		// Possible quick keys after prev tables.
  table_map const_tables,read_tables;
  /* See PARAM::possible_keys */
  key_map possible_keys;
  bool	free_cond; /* Currently not used and always FALSE */

  SQL_SELECT();
  ~SQL_SELECT();
  void cleanup();
  void set_quick(QUICK_SELECT_I *new_quick) { delete quick; quick= new_quick; }
  bool check_quick(THD *thd, bool force_quick_range, ha_rows limit)
  {
    key_map tmp;
    tmp.set_all();
    return test_quick_select(thd, tmp, 0, limit, force_quick_range,
                             FALSE, FALSE, FALSE) < 0;
  }
  /* 
    RETURN
      0   if record must be skipped <-> (cond && cond->val_int() == 0)
     -1   if error
      1   otherwise
  */   
  inline int skip_record(THD *thd)
  {
    int rc= MY_TEST(!cond || cond->val_int());
    if (thd->is_error())
      rc= -1;
    return rc;
  }
  int test_quick_select(THD *thd, key_map keys, table_map prev_tables,
			ha_rows limit, bool force_quick_range, 
                        bool ordered_output, bool remove_false_parts_of_where,
                        bool only_single_index_range_scan);
};


class SQL_SELECT_auto
{
  SQL_SELECT *select;
public:
  SQL_SELECT_auto(): select(NULL)
  {}
  ~SQL_SELECT_auto()
  {
    delete select;
  }
  SQL_SELECT_auto&
  operator= (SQL_SELECT *_select)
  {
    select= _select;
    return *this;
  }
  operator SQL_SELECT * () const
  {
    return select;
  }
  SQL_SELECT *
  operator-> () const
  {
    return select;
  }
  operator bool () const
  {
    return select;
  }
};


class FT_SELECT: public QUICK_RANGE_SELECT 
{
public:
  FT_SELECT(THD *thd, TABLE *table, uint key, bool *create_err) :
      QUICK_RANGE_SELECT (thd, table, key, 1, NULL, create_err) 
  { (void) init(); }
  ~FT_SELECT() { file->ft_end(); }
  virtual QUICK_RANGE_SELECT *clone(bool *create_error)
    { DBUG_ASSERT(0); return new FT_SELECT(thd, head, index, create_error); }
  int init() { return file->ft_init(); }
  int reset() { return 0; }
  int get_next() { return file->ha_ft_read(record); }
  int get_type() { return QS_TYPE_FULLTEXT; }
};

FT_SELECT *get_ft_select(THD *thd, TABLE *table, uint key);
QUICK_RANGE_SELECT *get_quick_select_for_ref(THD *thd, TABLE *table,
                                             struct st_table_ref *ref,
                                             ha_rows records);
SQL_SELECT *make_select(TABLE *head, table_map const_tables,
			table_map read_tables, COND *conds,
                        SORT_INFO* filesort,
                        bool allow_null_cond,  int *error);

bool calculate_cond_selectivity_for_table(THD *thd, TABLE *table, Item **cond);

bool eq_ranges_exceeds_limit(RANGE_SEQ_IF *seq, void *seq_init_param,
                             uint limit);

#ifdef WITH_PARTITION_STORAGE_ENGINE
bool prune_partitions(THD *thd, TABLE *table, Item *pprune_cond);
#endif
void store_key_image_to_rec(Field *field, uchar *ptr, uint len);

extern String null_string;

/* check this number of rows (default value) */
#define SELECTIVITY_SAMPLING_LIMIT 100
/* but no more then this part of table (10%) */
#define SELECTIVITY_SAMPLING_SHARE 0.10
/* do not check if we are going check less then this number of records */
#define SELECTIVITY_SAMPLING_THRESHOLD 10

#endif
