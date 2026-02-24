#pragma once
#include "my_global.h"
#include "datasink.h"
#include "aria_backup_client.h"
#include <string>
#include <memory>
#include <vector>
#include <unordered_map>

namespace ddl_log {

typedef std::unordered_map<std::string, std::string> tables_t;
bool backup(const char *datadir_path, ds_ctxt_t *ds, const tables_t &tables);

} // namespace ddl_log
