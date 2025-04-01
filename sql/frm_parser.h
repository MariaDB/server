#ifndef FRM_PARSER_H
#define FRM_PARSER_H

#include "my_global.h"

int read_frm_file(const char *name, const uchar **frmdata, size_t *len);

#endif
