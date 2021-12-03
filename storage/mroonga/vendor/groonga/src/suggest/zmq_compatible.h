/* -*- c-basic-offset: 2 -*- */
/* Copyright(C) 2013 Brazil

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
#ifndef GRN_SUGGEST_ZMQ_COMPATIBLE_H
#define GRN_SUGGEST_ZMQ_COMPATIBLE_H

#include <zmq.h>

#ifndef ZMQ_SNDHWM
#   define ZMQ_SNDHWM ZMQ_HWM
#endif

#if ZMQ_VERSION_MAJOR == 2
#  define zmq_msg_send(message, socket, flags) \
  zmq_send((socket), (message), (flags))
#  define zmq_msg_recv(message, socket, flags) \
  zmq_recv((socket), (message), (flags))
#endif

#endif /* GRN_SUGGEST_ZMQ_COMPATIBLE_H */
