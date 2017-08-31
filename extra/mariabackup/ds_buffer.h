/******************************************************
Copyright (c) 2012-2013 Percona LLC and/or its affiliates.

buffer datasink for XtraBackup.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA

*******************************************************/

#ifndef DS_BUFFER_H
#define DS_BUFFER_H

#include "datasink.h"

#ifdef __cplusplus
extern "C" {
#endif

extern datasink_t datasink_buffer;

/* Change the default buffer size */
void ds_buffer_set_size(ds_ctxt_t *ctxt, size_t size);

#ifdef __cplusplus
}
#endif

#endif
