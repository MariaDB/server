#pragma once

#include <string>
#include <vector>
#include <privilege.h>
#include <sql_acl.h>

/**
  deny entry used as input and output of closure routines.

  Identity is (type, db, table, column).

  Field semantics:
    db:
      Database name. Empty for GLOBAL.
    table:
      Object name within the database for object-level types
      (table, routine, package). Empty for DB and GLOBAL.
    column:
      Column name for COLUMN only. Empty otherwise.
    denies:
      Direct denies defined on this node.
    subtree_denies:
      Denies inherited from descendants only (does not include denies).

  Input rule:
    subtree_denies is ignored on input and computed by build_deny_closure().

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

typedef std::vector<deny_entry_t> deny_set_t;

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
deny_set_t build_deny_closure(const deny_set_t &input);

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
deny_set_t diff_deny_closure_inputs(const deny_set_t &old_input,
                                    const deny_set_t &new_input);

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
deny_set_t
diff_deny_closures(const deny_set_t &old_closure,
                   const deny_set_t &new_closure);


/**
  Match two deny entries by identity (type, db, table, column),
  using the correct collation for each field.
*/

bool deny_matches(ACL_PRIV_TYPE p1, const char *d1, const char *t1, const char *c1,
                  ACL_PRIV_TYPE p2, const char *d2, const char *t2, const char *c2);
