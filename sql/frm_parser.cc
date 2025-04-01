
#include "frm_parser.h"
#include "my_sys.h"
#include "mysqld_error.h"
#include "my_base.h"
#include "sql_error.h"
#include "sql_const.h"
#include "sql_class.h"              // For THD and other core SQL functions
#include "my_sys.h"                 // For low-level file functions
#include "my_base.h"                // For key_memory_frm_string, etc.
#include "mysql/psi/mysql_file.h"   // For mysql_file_open/read/close


#include <sys/stat.h>

int read_frm_file(const char *name, const uchar **frmdata, size_t *len) {
    File file;
    struct stat state;
    uchar *read_data;

    if (stat(name, &state) != 0)
        return 1;

    file = mysql_file_open(key_file_frm, name, O_RDONLY, MYF(0));
    if (file < 0)
        return 2;
    read_data = (uchar *)my_malloc(key_memory_frm_string, state.st_size, MYF(MY_WME));
    if (!read_data)
        return 3;

    if (mysql_file_read(file, read_data, (size_t)state.st_size, MYF(MY_NABP))) {
        my_free(read_data);
        return 4;
    }

    mysql_file_close(file, MYF(0));

    *frmdata = read_data;
    *len = state.st_size;
    return 0;
}
