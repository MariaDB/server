/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
/* -*- mode: C; c-basic-offset: 4 -*- */
#ident "$Id$"
/*======
This file is part of TokuDB


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    TokuDBis is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    TokuDB is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TokuDB.  If not, see <http://www.gnu.org/licenses/>.

======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#ifndef _TOKUDB_MEMORY_H
#define _TOKUDB_MEMORY_H

#include "hatoku_defines.h"

namespace tokudb {
namespace memory {

void* malloc(size_t s, myf flags);
void* realloc(void* p, size_t s, myf flags);
void free(void* ptr);
char* strdup(const char* p, myf flags);
void* multi_malloc(myf myFlags, ...);


inline void* malloc(size_t s, myf flags) {
#if 50700 <= MYSQL_VERSION_ID && MYSQL_VERSION_ID <= 50799
    return ::my_malloc(0, s, flags);
#else
    return ::my_malloc(s, flags);
#endif
}
inline void* realloc(void* p, size_t s, myf flags) {
    if (s == 0)
        return p;
#if 50700 <= MYSQL_VERSION_ID && MYSQL_VERSION_ID <= 50799
    return ::my_realloc(0, p, s, flags);
#else
    return ::my_realloc(p, s, flags | MY_ALLOW_ZERO_PTR);
#endif
}
inline void free(void* ptr) {
    if (ptr)
        ::my_free(ptr);
}
inline char* strdup(const char* p, myf flags) {
#if 50700 <= MYSQL_VERSION_ID && MYSQL_VERSION_ID <= 50799
    return ::my_strdup(0, p, flags);
#else
    return ::my_strdup(p, flags);
#endif
}
inline void* multi_malloc(myf myFlags, ...) {
    va_list args;
    char** ptr;
    char* start;
    char* res;
    size_t tot_length,length;

    va_start(args,myFlags);
    tot_length = 0;
    while ((ptr = va_arg(args, char**))) {
        length = va_arg(args, uint);
        tot_length += ALIGN_SIZE(length);
    }
    va_end(args);

    if (!(start = (char*)malloc(tot_length, myFlags))) {
        return 0;
    }

    va_start(args, myFlags);
    res = start;
    while ((ptr = va_arg(args, char**))) {
        *ptr = res;
        length = va_arg(args,uint);
        res += ALIGN_SIZE(length);
    }
    va_end(args);
    return start;
}

} // namespace thread
} // namespace tokudb

#endif // _TOKUDB_MEMORY_H
