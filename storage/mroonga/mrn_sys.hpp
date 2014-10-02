/*
  Copyright(C) 2010 Tetsuro IKEDA
  Copyright(C) 2011 Kentoku SHIBA
  Copyright(C) 2011-2012 Kouhei Sutou <kou@clear-code.com>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifndef MRN_SYS_HPP_
#define MRN_SYS_HPP_

#include <groonga.h>
#include "mrn_macro.hpp"

MRN_BEGIN_DECLS

/* functions */
bool mrn_hash_put(grn_ctx *ctx, grn_hash *hash, const char *key, grn_obj *value);
bool mrn_hash_get(grn_ctx *ctx, grn_hash *hash, const char *key, grn_obj **value);
bool mrn_hash_remove(grn_ctx *ctx, grn_hash *hash, const char *key);

MRN_END_DECLS

#endif /* MRN_SYS_HPP_ */
