#include "deny_closure.h"

#include <algorithm>


/* Under DB, in hierachy level, no children */
static inline bool is_db_leaf_type(ACL_PRIV_TYPE t)
{
  return (t == PRIV_TYPE_FUNCTION) || (t == PRIV_TYPE_PROCEDURE) ||
         (t == PRIV_TYPE_PACKAGE)  || (t == PRIV_TYPE_PACKAGE_BODY);
}

static inline int cmplen(size_t len1, size_t len2)
{
  if (len1 < len2)
    return -1;
  else if (len1 > len2)
    return 1;
  else
    return 0;
}

/*
  Compare according to lowercase_table_name rules
  (case-sensitive or case-insensitive depending on the variable).
*/
static int compare_fs(const LEX_CSTRING &s1, const LEX_CSTRING &s2)
{
  if (!s1.str || !s2.str)
    return cmplen(s1.length, s2.length);
  return Lex_ident_fs::charset_info()->strnncoll(s1.str, s1.length, s2.str,
                                                 s2.length);
}

/*
 Compare according to case-insensitive rules.
 */
static int compare_ci(const LEX_CSTRING &s1, const LEX_CSTRING &s2)
{
  if (!s1.str || !s2.str)
    return cmplen(s1.length, s2.length);
  return Lex_ident_ci::charset_info()->strnncoll(s1.str, s1.length, s2.str,
                                                 s2.length);
}

/*
  Compare 2 deny entries represented by ACL_PRIV type
  and db, table, column combo.
*/
static int compare(
 ACL_PRIV_TYPE p1, const LEX_CSTRING &d1,const LEX_CSTRING &t1, const LEX_CSTRING &c1,
 ACL_PRIV_TYPE p2, const LEX_CSTRING &d2,const LEX_CSTRING &t2, const LEX_CSTRING &c2)
{
  if (p1 != p2)
    return p1 < p2 ? -1 : 1;
  if (p1 == PRIV_TYPE_GLOBAL)
    return 0;
  int cmp= compare_fs(d1, d2);
  if (cmp)
    return cmp;
  if (p1 == PRIV_TYPE_DB)
    return 0;
  if (is_db_leaf_type(p1))
    return compare_ci(t1, t2);
  cmp= compare_fs(t1, t2);
  if (cmp || p1 == PRIV_TYPE_TABLE)
    return cmp;
  DBUG_ASSERT(p1 == PRIV_TYPE_COLUMN);
  return compare_ci(c1, c2);
  return 0;
}

/*
  Compare 2 deny entries for equality,
  each type is represented by ACL_PRIV type, db, table(or routine), column combo
*/
bool deny_matches( ACL_PRIV_TYPE p1, const char* d1, const char *t1, const char *c1,
 ACL_PRIV_TYPE p2, const char *d2,const char *t2, const char *c2)
{
  return compare(
      p1, Lex_cstring_strlen(d1),
      Lex_cstring_strlen(t1),Lex_cstring_strlen(c1),
      p2, Lex_cstring_strlen(d2), Lex_cstring_strlen(t2),
      Lex_cstring_strlen(c2)) == 0;
}

static inline LEX_CSTRING to_lex(const std::string &s)
{
  return {s.c_str(), s.length()};
}

static inline bool entry_less(const deny_entry_t &a,
                              const deny_entry_t &b)
{
  return compare(a.type, to_lex(a.db), to_lex(a.table), to_lex(a.column),
                 b.type, to_lex(b.db), to_lex(b.table), to_lex(b.column)) < 0;

}

struct node
{
  deny_entry_t data;
  node *parent;
  node *first_child;
  node *next_sibling;
};

static inline bool node_less(const node *n,
                             ACL_PRIV_TYPE type,
                             const std::string &db,
                             const std::string &table,
                             const std::string &column)
{
  return compare(
      n->data.type, to_lex(n->data.db), to_lex(n->data.table), to_lex(n->data.column),
      type, to_lex(db), to_lex(table), to_lex(column)) < 0;
}

static inline bool node_matches(const node *n,
                                ACL_PRIV_TYPE type,
                                const std::string &db,
                                const std::string &table,
                                const std::string &column)
{
  return compare(
      n->data.type, to_lex(n->data.db), to_lex(n->data.table), to_lex(n->data.column),
      type, to_lex(db), to_lex(table), to_lex(column)) == 0;
}

namespace {

class deny_closure_builder
{
public:
  deny_closure_builder()
    : m_root(NULL), m_built(false)
  {
    reset();
  }

  ~deny_closure_builder()
  {
    clear_nodes();
  }

  void reset()
  {
    clear_nodes();
    m_nodes.reserve(16);
    m_root= intern_node(PRIV_TYPE_GLOBAL, "", "", "");
    m_built= false;
  }

  void add(const deny_entry_t &in)
  {
    ACL_PRIV_TYPE type= in.type;

    intern_node(PRIV_TYPE_GLOBAL, "", "", "");

    if (type == PRIV_TYPE_GLOBAL)
    {
      intern_node(PRIV_TYPE_GLOBAL, "", "", "")->data.denies|= in.denies;
      return;
    }

    if (type == PRIV_TYPE_DB)
    {
      intern_node(PRIV_TYPE_DB, in.db, "", "")->data.denies|= in.denies;
      return;
    }

    if (type == PRIV_TYPE_TABLE)
    {
      intern_node(PRIV_TYPE_DB, in.db, "", "");
      intern_node(PRIV_TYPE_TABLE, in.db, in.table, "")->data.denies|= in.denies;
      return;
    }

    if (type == PRIV_TYPE_COLUMN)
    {
      intern_node(PRIV_TYPE_DB, in.db, "", "");
      intern_node(PRIV_TYPE_TABLE, in.db, in.table, "");
      intern_node(PRIV_TYPE_COLUMN, in.db, in.table, in.column)->data.denies|= in.denies;
      return;
    }

    if (is_db_leaf_type(type))
    {
      intern_node(PRIV_TYPE_DB, in.db, "", "");
      intern_node(type, in.db, in.table, "")->data.denies|= in.denies;
      return;
    }

    /* Unknown type: ignore (or assert). */
  }

  void add_all(const deny_set_t &v)
  {
    for (const deny_entry_t &d : v)
      add(d);
  }

  deny_set_t finish()
  {
    build_tree_if_needed();
    compute_subtree_denies(m_root);

    deny_set_t out;
    out.reserve(m_nodes.size());
    flatten(m_root, out);

    std::sort(out.begin(), out.end(), entry_less);


    if (out.size() == 1 &&
      out[0].type == PRIV_TYPE_GLOBAL &&
      !out[0].denies &&
      !out[0].subtree_denies)
    {
     out.clear();
    }
    return out;
  }

private:
  node *intern_node(ACL_PRIV_TYPE type,
                         const std::string &db,
                         const std::string &name,
                         const std::string &column)
  {
    auto it= std::lower_bound(
      m_nodes.begin(), m_nodes.end(), type,
      [&](const node *n, ACL_PRIV_TYPE)
      {
        return node_less(n, type, db, name, column);
      });

    if (it != m_nodes.end() && node_matches(*it, type, db, name, column))
      return *it;

    node *n= new node;
    n->data.type= type;
    n->data.db= db;
    n->data.table= name;
    n->data.column= column;
    n->data.denies= NO_ACL;
    n->data.subtree_denies= NO_ACL;
    n->parent= NULL;
    n->first_child= NULL;
    n->next_sibling= NULL;

    m_nodes.insert(it, n);
    m_built= false;
    return n;
  }

  node *find_parent(node *n)
  {
    const deny_entry_t &d= n->data;
    if (d.type == PRIV_TYPE_DB)
      return m_root;

    if (d.type == PRIV_TYPE_TABLE || is_db_leaf_type(d.type))
      return intern_node(PRIV_TYPE_DB, d.db, "", "");

    if (d.type == PRIV_TYPE_COLUMN)
      return intern_node(PRIV_TYPE_TABLE, d.db, d.table, "");

    return NULL;
  }

  void attach_child(node *parent, node *child)
  {
    child->parent= parent;
    child->next_sibling= parent->first_child;
    parent->first_child= child;
  }

  void build_tree_if_needed()
  {
    if (m_built)
      return;

    for (node *n : m_nodes)
    {
      n->parent= NULL;
      n->first_child= NULL;
      n->next_sibling= NULL;
      n->data.subtree_denies= NO_ACL;
    }

    m_root= intern_node(PRIV_TYPE_GLOBAL, "", "", "");

    size_t i= 0;
    while (i < m_nodes.size())
    {
      node *n= m_nodes[i++];
      if (n == m_root)
        continue;

      node *p= find_parent(n);
      if (p)
        attach_child(p, n);
    }

    m_built= true;
  }

  privilege_t compute_subtree_denies(node *n)
  {
    privilege_t acc= NO_ACL;

    for (node *c= n->first_child; c; c= c->next_sibling)
      acc|= (c->data.denies | compute_subtree_denies(c));

    n->data.subtree_denies= acc;
    return acc;
  }

  void flatten(node *n, deny_set_t &out)
  {
    out.push_back(n->data);

    for (node *c= n->first_child; c; c= c->next_sibling)
      flatten(c, out);
  }

  void clear_nodes()
  {
    for (node *n : m_nodes)
      delete n;

    m_nodes.clear();
    m_root= NULL;
    m_built= false;
  }

private:
  std::vector<node *> m_nodes;
  node *m_root;
  bool m_built;
};

} /* anonymous namespace */

deny_set_t
build_deny_closure(const deny_set_t &input)
{
  deny_closure_builder b;
  b.add_all(input);
  return b.finish();
}

deny_set_t
diff_deny_closures(const deny_set_t &old_closure_in,
                   const deny_set_t &new_closure_in)
{
  deny_set_t old_cl= old_closure_in;
  deny_set_t new_cl= new_closure_in;

  std::sort(old_cl.begin(), old_cl.end(), entry_less);
  std::sort(new_cl.begin(), new_cl.end(), entry_less);

  deny_set_t delta;

  size_t i= 0;
  size_t j= 0;

  while (i < old_cl.size() || j < new_cl.size())
  {
    if (i == old_cl.size())
    {
      delta.push_back(new_cl[j++]); /* added */
      continue;
    }

    if (j == new_cl.size())
    {
      deny_entry_t gone= old_cl[i++];
      gone.denies= NO_ACL;
      gone.subtree_denies= NO_ACL;
      delta.push_back(gone); /* removed */
      continue;
    }

    const deny_entry_t &a= old_cl[i];
    const deny_entry_t &b= new_cl[j];

    if (entry_less(a, b))
    {
      deny_entry_t gone= a;
      gone.denies= NO_ACL;
      gone.subtree_denies= NO_ACL;
      delta.push_back(gone); /* removed */
      ++i;
      continue;
    }

    if (entry_less(b, a))
    {
      delta.push_back(b); /* added */
      ++j;
      continue;
    }

    if (a.denies != b.denies || a.subtree_denies != b.subtree_denies)
      delta.push_back(b); /* changed, include into delta */

    ++i;
    ++j;
  }

  return delta;
}

deny_set_t
diff_deny_closure_inputs(const deny_set_t &old_input,
                         const deny_set_t &new_input)
{
  deny_set_t old_cl= build_deny_closure(old_input);
  deny_set_t new_cl= build_deny_closure(new_input);
  return diff_deny_closures(old_cl, new_cl);
}
