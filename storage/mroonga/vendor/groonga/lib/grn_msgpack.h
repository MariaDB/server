/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2009-2016 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#pragma once

#ifdef GRN_WITH_MESSAGE_PACK
# include <msgpack.h>

# if MSGPACK_VERSION_MAJOR < 1
typedef unsigned int msgpack_size_t;

#  define msgpack_pack_str(packer, size) msgpack_pack_raw(packer, size)
#  define msgpack_pack_str_body(packer, value, size) \
  msgpack_pack_raw_body(packer, value, size)

#  define MSGPACK_OBJECT_STR MSGPACK_OBJECT_RAW
#  define MSGPACK_OBJECT_FLOAT MSGPACK_OBJECT_DOUBLE

#  define MSGPACK_OBJECT_STR_PTR(object)  (object)->via.raw.ptr
#  define MSGPACK_OBJECT_STR_SIZE(object) (object)->via.raw.size

#  define MSGPACK_OBJECT_FLOAT_VALUE(object) (object)->via.dec
# else /* MSGPACK_VERSION_MAJOR < 1 */
typedef size_t msgpack_size_t;

#  define MSGPACK_OBJECT_STR_PTR(object)  (object)->via.str.ptr
#  define MSGPACK_OBJECT_STR_SIZE(object) (object)->via.str.size

#  define MSGPACK_OBJECT_FLOAT_VALUE(object) (object)->via.f64
# endif /* MSGPACK_VERSION_MAJOR < 1 */
#endif /* GRN_WITH_MESSAGE_PACK */
