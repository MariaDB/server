#include <my_config.h>


/* MySQL includes */
#include "./debug_sync.h"
#include "./my_bit.h"
#include "./my_stacktrace.h"
#include "./sql_table.h"
#include "./my_global.h"
#include "./log.h"
#include <mysys_err.h>
#include <mysql/psi/mysql_table.h>
//#include <mysql/thread_pool_priv.h>

#include <string>

/* MyRocks includes */
#include "./rdb_threads.h"

#include "rdb_mariadb_server_port.h"

void warn_about_bad_patterns(const Regex_list_handler* regex_list_handler,
                             const char *name)
{
  // There was some invalid regular expression data in the patterns supplied

  // NO_LINT_DEBUG
  sql_print_warning("Invalid pattern in %s: %s", name,
                    regex_list_handler->bad_pattern().c_str());
}


/*
  Set the patterns string.  If there are invalid regex patterns they will
  be stored in m_bad_patterns and the result will be false, otherwise the
  result will be true.
*/
bool Regex_list_handler::set_patterns(const std::string& pattern_str)
{
  bool pattern_valid= true;

  // Create a normalized version of the pattern string with all delimiters
  // replaced by the '|' character
  std::string norm_pattern= pattern_str;
  std::replace(norm_pattern.begin(), norm_pattern.end(), m_delimiter, '|');

  // Make sure no one else is accessing the list while we are changing it.
  mysql_rwlock_wrlock(&m_rwlock);

  // Clear out any old error information
  m_bad_pattern_str.clear();

  try
  {
    // Replace all delimiters with the '|' operator and create the regex
    // Note that this means the delimiter can not be part of a regular
    // expression.  This is currently not a problem as we are using the comma
    // character as a delimiter and commas are not valid in table names.
    const std::regex* pattern= new std::regex(norm_pattern);

    // Free any existing regex information and setup the new one
    delete m_pattern;
    m_pattern= pattern;
  }
  catch (const std::regex_error&)
  {
    // This pattern is invalid.
    pattern_valid= false;

    // Put the bad pattern into a member variable so it can be retrieved later.
    m_bad_pattern_str= pattern_str;
  }

  // Release the lock
  mysql_rwlock_unlock(&m_rwlock);

  return pattern_valid;
}

bool Regex_list_handler::matches(const std::string& str) const
{
  DBUG_ASSERT(m_pattern != nullptr);

  // Make sure no one else changes the list while we are accessing it.
  mysql_rwlock_rdlock(&m_rwlock);

  // See if the table name matches the regex we have created
  bool found= std::regex_match(str, *m_pattern);

  // Release the lock
  mysql_rwlock_unlock(&m_rwlock);

  return found;
}

// Split a string based on a delimiter.  Two delimiters in a row will not add
// an empty string in the set.
std::vector<std::string> split_into_vector(const std::string& input,
                                           char delimiter)
{
  size_t                   pos;
  size_t                   start = 0;
  std::vector<std::string> elems;

  // Find next delimiter
  while ((pos = input.find(delimiter, start)) != std::string::npos)
  {
    // If there is any data since the last delimiter add it to the list
    if (pos > start)
      elems.push_back(input.substr(start, pos - start));

    // Set our start position to the character after the delimiter
    start = pos + 1;
  }

  // Add a possible string since the last delimiter
  if (input.length() > start)
    elems.push_back(input.substr(start));

  // Return the resulting list back to the caller
  return elems;
}

