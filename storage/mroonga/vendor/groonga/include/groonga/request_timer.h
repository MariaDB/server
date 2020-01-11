/*
  Copyright(C) 2016 Brazil

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
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#pragma once

#ifdef  __cplusplus
extern "C" {
#endif

typedef struct _grn_request_timer {
  void *user_data;
  void *(*register_func)(const char *request_id,
                         unsigned int request_id_size,
                         double timeout,
                         void *user_data);
  void (*unregister_func)(void *timer_id,
                          void *user_data);
  void (*fin_func)(void *user_data);
} grn_request_timer;

/* Multithreading unsafe. */
GRN_API void grn_request_timer_set(grn_request_timer *timer);

/* Multithreading safety is depends on grn_request_timer. */
GRN_API void *grn_request_timer_register(const char *request_id,
                                         unsigned int request_id_size,
                                         double timeout);
/* Multithreading safety is depends on grn_request_timer. */
GRN_API void grn_request_timer_unregister(void *timer_id);


GRN_API double grn_get_default_request_timeout(void);
GRN_API void grn_set_default_request_timeout(double timeout);


#ifdef __cplusplus
}
#endif
