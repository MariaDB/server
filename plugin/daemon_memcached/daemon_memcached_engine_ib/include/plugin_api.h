#ifndef PLUGIN_API_H
#define PLUGIN_API_H

#include "innodb_api.h"

#ifdef  __cplusplus
extern "C" {
#endif

ib_cb_t** obtain_innodb_cb();

#ifdef  __cplusplus
}
#endif

#endif  /* PLUGIN_API_H */
