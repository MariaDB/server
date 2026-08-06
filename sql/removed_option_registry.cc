#include "removed_option_registry.h"

#include <mutex>
#include <new>
#include <vector>

static std::vector<Removed_startup_option> g_removed_startup_options;
static std::mutex g_removed_startup_options_lock;

bool init_removed_startup_option_registry()
{
  return false;
}

void free_removed_startup_option_registry()
{
  std::lock_guard<std::mutex> guard(g_removed_startup_options_lock);
  g_removed_startup_options.clear();
  g_removed_startup_options.shrink_to_fit();
}

bool register_removed_startup_option(const char *name,
                                     const char *value,
                                     const char *filename)
{
  Removed_startup_option row;

  row.option_name= name ? name : "";
  row.option_value= value ? value : "";

  if (filename && *filename)
  {
    row.source= "CONFIG";
    row.config_file= filename;
  }
  else
  {
    row.source= "COMMAND_LINE";
    row.config_file.clear();
  }

 row.handling= "IGNORED";

 try
  {
    std::lock_guard<std::mutex> guard(g_removed_startup_options_lock);
    g_removed_startup_options.push_back(row);
  }
  catch (const std::bad_alloc &)
  {
    return true;
  }

  return false;
}

size_t removed_startup_option_count()
{
  std::lock_guard<std::mutex> guard(g_removed_startup_options_lock);
  return g_removed_startup_options.size();
}

bool get_removed_startup_option_copy(size_t index, Removed_startup_option *out)
{
  if (!out)
    return true;

  std::lock_guard<std::mutex> guard(g_removed_startup_options_lock);

  if (index >= g_removed_startup_options.size())
    return true;

  *out= g_removed_startup_options[index];
  return false;
}
