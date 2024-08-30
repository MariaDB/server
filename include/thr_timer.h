/* Copyright (c) 2014 Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 or later of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/* Prototypes when using thr_timer functions */

#ifndef THR_TIMER_INCLUDED
#define THR_TIMER_INCLUDED
#ifdef	__cplusplus
extern "C" {
#endif

typedef struct st_timer {
  struct timespec expire_time;
  ulonglong period;
  my_bool expired;
  uint index_in_queue;
  void (*func)(void*);
  void *func_arg;
} thr_timer_t;

/* Main functions for library */
my_bool init_thr_timer(uint init_size_for_timer_queue);
void end_thr_timer();

/* Functions for handling one timer */
void thr_timer_init(thr_timer_t *timer_data, void(*function)(void*),
                    void *arg);
void thr_timer_set_period(thr_timer_t* timer_data, ulonglong microseconds);
my_bool thr_timer_settime(thr_timer_t *timer_data, ulonglong microseconds);
void    thr_timer_end(thr_timer_t *timer_data);

#ifdef	__cplusplus
}
#endif /* __cplusplus */
#endif /* THR_TIMER_INCLUDED */
