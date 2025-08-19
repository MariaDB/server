/* Copyright (c) 2000, 2015, Oracle and/or its affiliates.
   Copyright (c) 2008, 2020, MariaDB

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

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include "mariadb.h"
#include "sql_priv.h"
#include "key.h"        // is_key_used, key_copy, key_cmp, key_restore
#include "sql_parse.h"                          // check_stack_overrun
#include "sql_partition.h"    // get_part_id_func, PARTITION_ITERATOR,
                              // struct partition_info, NOT_A_PARTITION_ID
#include "records.h"          // init_read_record, end_read_record
#include <m_ctype.h>
#include "sql_select.h"
#include "sql_statistics.h"
#include "uniques.h"
#include "my_json_writer.h"


class SEL_IMERGE;

#define CLONE_KEY1_MAYBE 1
#define CLONE_KEY2_MAYBE 2
#define swap_clone_flag(A) ((A & 1) << 1) | ((A & 2) >> 1)


/*
  While objects of the class SEL_ARG represent ranges for indexes or
  index infixes (including ranges for index prefixes and index suffixes),
  objects of the class SEL_TREE represent AND/OR formulas of such ranges.
  Currently an AND/OR formula represented by a SEL_TREE object can have
  at most three levels: 

    <SEL_TREE formula> ::= 
      [ <SEL_RANGE_TREE formula> AND ]
      [ <SEL_IMERGE formula> [ AND <SEL_IMERGE formula> ...] ]

    <SEL_RANGE_TREE formula> ::=
      <SEL_ARG formula> [ AND  <SEL_ARG_formula> ... ]

    <SEL_IMERGE formula> ::=  
      <SEL_RANGE_TREE formula> [ OR <SEL_RANGE_TREE formula> ]

  As we can see from the above definitions:
   - SEL_RANGE_TREE formula is a conjunction of SEL_ARG formulas
   - SEL_IMERGE formula is a disjunction of SEL_RANGE_TREE formulas
   - SEL_TREE formula is a conjunction of a SEL_RANGE_TREE formula
     and SEL_IMERGE formulas. 
  It's required above that a SEL_TREE formula has at least one conjunct.

  Usually we will consider normalized SEL_RANGE_TREE formulas where we use
  TRUE as conjunct members for those indexes whose SEL_ARG trees are empty.
  
  We will call an SEL_TREE object simply 'tree'. 
  The part of a tree that represents SEL_RANGE_TREE formula is called
  'range part' of the tree while the remaining part is called 'imerge part'. 
  If a tree contains only a range part then we call such a tree 'range tree'.
  Components of a range tree that represent SEL_ARG formulas are called ranges.
  If a tree does not contain any range part we call such a tree 'imerge tree'.
  Components of the imerge part of a tree that represent SEL_IMERGE formula
  are called imerges.

  Usually we'll designate:
    SEL_TREE formulas         by T_1,...,T_k
    SEL_ARG formulas          by R_1,...,R_k
    SEL_RANGE_TREE formulas   by RT_1,...,RT_k
    SEL_IMERGE formulas       by M_1,...,M_k
  Accordingly we'll use:
    t_1,...,t_k - to designate trees representing T_1,...,T_k
    r_1,...,r_k - to designate ranges representing R_1,...,R_k 
    rt_1,...,r_tk - to designate range trees representing RT_1,...,RT_k
    m_1,...,m_k - to designate imerges representing M_1,...,M_k

  SEL_TREE objects are usually built from WHERE conditions or
  ON expressions.
  A SEL_TREE object always represents an inference of the condition it is
  built from. Therefore, if a row satisfies a SEL_TREE formula it also
  satisfies the condition it is built from.

  The following transformations of tree t representing SEL_TREE formula T 
  yield a new tree t1 thar represents an inference of T: T=>T1.  
    (1) remove any of SEL_ARG tree from the range part of t
    (2) remove any imerge from the tree t 
    (3) remove any of SEL_ARG tree from any range tree contained
        in any imerge of tree   
 
  Since the basic blocks of any SEL_TREE objects are ranges, SEL_TREE
  objects in many cases can be effectively used to filter out a big part
  of table rows that do not satisfy WHERE/IN conditions utilizing
  only single or multiple range index scans.

  A single range index scan is constructed for a range tree that contains
  only one SEL_ARG object for an index or an index prefix.
  An index intersection scan can be constructed for a range tree
  that contains several SEL_ARG objects. Currently index intersection
  scans are constructed only for single-point ranges.
  An index merge scan is constructed for a imerge tree that contains only
  one imerge. If range trees of this imerge contain only single-point merges
  than a union of index intersections can be built.

  Usually the tree built by the range optimizer for a query table contains
  more than one range in the range part, and additionally may contain some
  imerges in the imerge part. The range optimizer evaluates all of them one
  by one and chooses the range or the imerge that provides the cheapest
  single or multiple range index scan of the table.  According to rules 
  (1)-(3) this scan always filter out only those rows that do not satisfy
  the query conditions. 

  For any condition the SEL_TREE object for it is built in a bottom up
  manner starting from the range trees for the predicates. The tree_and
  function builds a tree for any conjunction of formulas from the trees
  for its conjuncts. The tree_or function builds a tree for any disjunction
  of formulas from the trees for its disjuncts.    
*/ 
  
class SEL_TREE :public Sql_alloc
{
public:
  /*
    Starting an effort to document this field:
    (for some i, keys[i]->type == SEL_ARG::IMPOSSIBLE) => 
       (type == SEL_TREE::IMPOSSIBLE)
  */
  enum Type { IMPOSSIBLE, ALWAYS, MAYBE, KEY, KEY_SMALLER } type;

  SEL_TREE(enum Type type_arg, MEM_ROOT *root, size_t num_keys)
    : type(type_arg), keys(root, num_keys), n_ror_scans(0)
  {
    keys_map.clear_all();
  }

  SEL_TREE(MEM_ROOT *root, size_t num_keys) :
    type(KEY), keys(root, num_keys), n_ror_scans(0)
  { 
    keys_map.clear_all();
  }
   
  SEL_TREE(SEL_TREE *arg, bool without_merges, RANGE_OPT_PARAM *param);
  /*
    Note: there may exist SEL_TREE objects with sel_tree->type=KEY and
    keys[i]=0 for all i. (SergeyP: it is not clear whether there is any
    merit in range analyzer functions (e.g. get_mm_parts) returning a
    pointer to such SEL_TREE instead of NULL)
  */
  Mem_root_array<SEL_ARG *, true> keys;
  key_map keys_map;        /* bitmask of non-NULL elements in keys */

  /*
    Possible ways to read rows using index_merge. The list is non-empty only
    if type==KEY. Currently can be non empty only if keys_map.is_clear_all().
  */
  List<SEL_IMERGE> merges;

  /* The members below are filled/used only after get_mm_tree is done */
  key_map ror_scans_map;   /* bitmask of ROR scan-able elements in keys */
  uint    n_ror_scans;     /* number of set bits in ror_scans_map */

  struct st_index_scan_info **index_scans;     /* list of index scans */
  struct st_index_scan_info **index_scans_end; /* last index scan */

  struct st_ror_scan_info **ror_scans;     /* list of ROR key scans */
  struct st_ror_scan_info **ror_scans_end; /* last ROR scan */
  /* Note that #records for each key scan is stored in table->quick_rows */

  bool without_ranges() { return keys_map.is_clear_all(); }
  bool without_imerges() { return merges.is_empty(); }

  key_map get_keys_map()
  {
    return keys_map;
  }

  SEL_ARG *get_key(uint keyno)
  {
    return keys[keyno];
  }

  uint get_type()
  {
    return (uint)type;
  }
};

