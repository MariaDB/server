#include <mysql.h>
#include <string>
extern void xb_plugin_backup_init(MYSQL *mysql);
extern const char* xb_plugin_get_config();
extern void xb_plugin_prepare_init(int argc, char **argv, const char *dir);
