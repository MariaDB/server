#include <mysql.h>
#include <string>
extern void encryption_plugin_backup_init(MYSQL *mysql);
extern const char* encryption_plugin_get_config();
extern void encryption_plugin_prepare_init(int argc, char **argv);

//extern  void encryption_plugin_init(int argc, char **argv);
