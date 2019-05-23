/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2016 Brazil

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

#include "grn_ctx.h"
#include "grn_request_timer.h"

static grn_request_timer grn_current_request_timer = { 0 };
static double grn_request_timer_default_timeout = 0.0;

grn_bool
grn_request_timer_init(void)
{
  return GRN_TRUE;
}

void *
grn_request_timer_register(const char *request_id,
                           unsigned int request_id_size,
                           double timeout)
{
  void *timer_id = NULL;

  if (grn_current_request_timer.register_func) {
    void *user_data = grn_current_request_timer.user_data;
    timer_id = grn_current_request_timer.register_func(request_id,
                                                       request_id_size,
                                                       timeout,
                                                       user_data);
  }

  return timer_id;
}

void
grn_request_timer_unregister(void *timer_id)
{
  if (grn_current_request_timer.unregister_func) {
    void *user_data = grn_current_request_timer.user_data;
    grn_current_request_timer.unregister_func(timer_id, user_data);
  }
}

void
grn_request_timer_set(grn_request_timer *timer)
{
  if (grn_current_request_timer.fin_func) {
    void *user_data = grn_current_request_timer.user_data;
    grn_current_request_timer.fin_func(user_data);
  }
  if (timer) {
    grn_current_request_timer = *timer;
  } else {
    memset(&grn_current_request_timer, 0, sizeof(grn_request_timer));
  }
}

double
grn_get_default_request_timeout(void)
{
  return grn_request_timer_default_timeout;
}

void
grn_set_default_request_timeout(double timeout)
{
  grn_request_timer_default_timeout = timeout;
}

void
grn_request_timer_fin(void)
{
  grn_request_timer_set(NULL);
}
