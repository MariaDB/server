#ifndef REMOVED_OPTION_REGISTRY_H
#define REMOVED_OPTION_REGISTRY_H

#include <stddef.h>
#include <string>

struct Removed_startup_option
{
  std::string option_name;
  std::string option_value;
  std::string source;       // "CONFIG" or "COMMAND_LINE"
  std::string config_file;  // empty if not from config file
  std::string handling;     // "IGNORED"
};

bool init_removed_startup_option_registry();
void free_removed_startup_option_registry();

bool register_removed_startup_option(const char *name,
                                     const char *value,
                                     const char *filename);

size_t removed_startup_option_count();
bool get_removed_startup_option_copy(size_t index, Removed_startup_option *out);

#endif
