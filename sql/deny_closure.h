#pragma once

#include <string>
#include <vector>
#include <privilege.h>
#include <sql_acl.h>

/**
  Canonical privilege entry used as input and output of closure routines.

  Identity is:
    (type, db, table, column)

  Field semantics:
    db:
      Database name. Empty only for GLOBAL.

    table:
      Object name within the database.

      For TABLE        : table name
      For COLUMN       : table name
      For FUNCTION     : routine name
      For PROCEDURE    : routine name
      For PACKAGE      : package name
      For PACKAGE_BODY : package body name

      Empty for DB and GLOBAL.

    column:
      Column name (only valid when type == COLUMN).
      Empty for all other types.

    denies:
      Direct denies defined on this node.

    subtree_denies:
      Denies inherited from descendants (children-only).
      Direct denies on the node are NOT included in subtree_denies.

  Input rule:
    subtree_denies is ignored on input. It is computed by build_deny_closure().

  @note This module is intended for low-frequency operations (startup, FLUSH
        PRIVILEGES, DENY/REVOKE DENY updates).
*/
struct deny_entry_t
{
  ACL_PRIV_TYPE type;
  std::string db;
  std::string table;
  std::string column;
  privilege_t denies;
  privilege_t subtree_denies;
};

/**
  Build canonical hierarchical closure for denies.

  The returned closure:
    - materializes implied parent nodes
    - computes subtree_denies (children-only)
    - is sorted canonically by identity:
        (type, db, table, column)

  @param input  Flat list. subtree_denies is ignored on input.
  @return       Canonical sorted closure.
*/
std::vector<deny_entry_t>
build_deny_closure(const std::vector<deny_entry_t> &input);

/**
  Compute difference between two deny states given as flat inputs.

  This builds canonical closures for both inputs and returns a delta.

  Delta encoding:
    - Added / Changed: "after" entry (from new state closure)
    - Removed        : same identity, denies= 0 and subtree_denies= 0

  Output is sorted canonically by identity.

  @param old_input  Old state (flat form). subtree_denies ignored.
  @param new_input  New state (flat form). subtree_denies ignored.
  @return           Sorted delta vector.
*/
std::vector<deny_entry_t>
diff_deny_closure_inputs(const std::vector<deny_entry_t> &old_input,
                         const std::vector<deny_entry_t> &new_input);

/**
  Compute difference between two already-built closures.

  Delta encoding:
    - Added / Changed: "after" entry (from new_closure)
    - Removed        : same identity, denies= 0 and subtree_denies= 0

  Output is sorted canonically by identity.

  @param old_closure  Old closure (sorted or unsorted).
  @param new_closure  New closure (sorted or unsorted).
  @return             Sorted delta vector.
*/
std::vector<deny_entry_t>
diff_deny_closures(const std::vector<deny_entry_t> &old_closure,
                   const std::vector<deny_entry_t> &new_closure);


/**
 Match two deny entries by identity (type, db, table, column).
 Uses correct collation for each field comparison.
*/

bool deny_matches(ACL_PRIV_TYPE p1, const char *d1, const char *t1, const char *c1,
                  ACL_PRIV_TYPE p2, const char *d2, const char *t2, const char *c2);
