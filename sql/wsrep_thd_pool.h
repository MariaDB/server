/* Copyright (C) 2015 Codership Oy <info@codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. */


#ifndef WSREP_THD_POOL_H
#define WSREP_THD_POOL_H

#include <cstddef>
#include <vector>

class THD;

class Wsrep_thd_pool
{
public:
    Wsrep_thd_pool(size_t threads = 10);
    ~Wsrep_thd_pool();
    THD* get_thd(THD*);
    void release_thd(THD*);
private:
    size_t threads_;
    std::vector<THD*> pool_;
};

#endif /* !WSREP_THD_POOL_H */
