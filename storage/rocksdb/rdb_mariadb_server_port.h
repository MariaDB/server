/*
  A temporary header to resolve WebScaleSQL vs MariaDB differences 
  when porting MyRocks to MariaDB.
*/
#ifndef RDB_MARIADB_SERVER_PORT_H
#define RDB_MARIADB_SERVER_PORT_H

#include "my_global.h"                   /* ulonglong */
#include "atomic_stat.h"
#include "my_pthread.h"
#include <mysql/psi/mysql_table.h>
#include <mysql/psi/mysql_thread.h>

/*
  Code that is on SQL layer in facebook/mysql-5.6,
  but is part of the storage engine in MariaRocks
*/
#include <regex>

class Regex_list_handler
{
 private:
#if defined(HAVE_PSI_INTERFACE)
  const PSI_rwlock_key& m_key;
#endif

  char m_delimiter;
  std::string m_bad_pattern_str;
  const std::regex* m_pattern;

  mutable mysql_rwlock_t m_rwlock;

  Regex_list_handler(const Regex_list_handler& other)= delete;
  Regex_list_handler& operator=(const Regex_list_handler& other)= delete;

 public:
#if defined(HAVE_PSI_INTERFACE)
  Regex_list_handler(const PSI_rwlock_key& key,
                     char delimiter= ',') :
    m_key(key),
#else
  Regex_list_handler(char delimiter= ',') :
#endif
    m_delimiter(delimiter),
    m_bad_pattern_str(""),
    m_pattern(nullptr)
  {
    mysql_rwlock_init(key, &m_rwlock);
  }

  ~Regex_list_handler()
  {
    mysql_rwlock_destroy(&m_rwlock);
    delete m_pattern;
  }

  // Set the list of patterns
  bool set_patterns(const std::string& patterns);

  // See if a string matches at least one pattern
  bool matches(const std::string& str) const;

  // See the list of bad patterns
  const std::string& bad_pattern() const
  {
    return m_bad_pattern_str;
  }
};

void warn_about_bad_patterns(const Regex_list_handler* regex_list_handler,
                             const char *name);

void print_keydup_error(TABLE *table, KEY *key, myf errflag,
                        const THD *thd, const char *org_table_name=NULL);

#endif
