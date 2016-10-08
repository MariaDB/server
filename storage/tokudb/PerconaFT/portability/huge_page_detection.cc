/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of PerconaFT.


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.

----------------------------------------

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License, version 3,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.
======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#include <sys/mman.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <portability/toku_assert.h>
#include <portability/toku_os.h>

static bool check_huge_pages_config_file(const char *fname)
// Effect: Return true if huge pages are there.  If so, print diagnostics.
{
    bool huge_pages_enabled = false;
    FILE *f = fopen(fname, "r");
    if (f) {
        // It's redhat and the feature appears to be there.  Is it enabled?
        char buf[1000];
        char *r = fgets(buf, sizeof(buf), f);
        assert(r != NULL);
        if (strstr(buf, "[always]")) {
            fprintf(stderr, "Transparent huge pages are enabled, according to %s\n", fname);
            huge_pages_enabled = true;
        } else {
            huge_pages_enabled =false;
        }
        fclose(f);
    }
    return huge_pages_enabled;
}

static bool check_huge_pages_in_practice(void)
// Effect: Return true if huge pages appear to be defined in practice.
{
#ifdef HAVE_MINCORE    
#ifdef HAVE_MAP_ANONYMOUS    
    const int map_anonymous = MAP_ANONYMOUS;
#else
    const int map_anonymous = MAP_ANON;
#endif
    const size_t TWO_MB = 2UL*1024UL*1024UL;

    void *first = mmap(NULL, 2*TWO_MB, PROT_READ|PROT_WRITE, MAP_PRIVATE|map_anonymous, -1, 0);
    if ((long)first==-1) perror("mmap failed");
    {
        int r = munmap(first, 2*TWO_MB);
        assert(r==0);
    }

    void *second_addr = (void*)(((unsigned long)first + TWO_MB) & ~(TWO_MB -1));
    void *second = mmap(second_addr, TWO_MB, PROT_READ|PROT_WRITE, MAP_FIXED|MAP_PRIVATE|map_anonymous, -1, 0);
    if ((long)second==-1) perror("mmap failed");
    assert((long)second%TWO_MB == 0);

    const long pagesize = 4096;
    const long n_pages = TWO_MB/pagesize;
#ifdef __linux__
    // On linux mincore is defined as mincore(void *, size_t, unsigned char *)
    unsigned char vec[n_pages];
#else
    // On BSD (OS X included) it is defined as mincore(void *, size_t, char *)
    char vec[n_pages];
#endif
    {
        int r = mincore(second, TWO_MB, vec);
        if (r!=0 && errno==ENOMEM) {
            // On some kernels (e.g., Centos 5.8), mincore doesn't work.  It seems unlikely that huge pages are here.
            munmap(second, TWO_MB);
            return false;
        }
        assert(r==0);
    }
    for (long i=0; i<n_pages; i++) {
        assert(!vec[i]);
    }
    ((char*)second)[0] = 1;
    {
        int r = mincore(second, TWO_MB, vec);
        // If the mincore worked the first time, it probably works here too.x
        assert(r==0);
    }
    assert(vec[0]);
    {
        int r = munmap(second, TWO_MB);
        assert(r==0);
    }
    if (vec[1]) {
        fprintf(stderr, "Transparent huge pages appear to be enabled according to mincore()\n");
        return true;
    } else {
        return false;
    }
#else
    // No mincore, so no way to check this in practice
    return false;
#endif
}

bool toku_os_huge_pages_enabled(void)
// Effect: Return true if huge pages appear to be enabled.  If so, print some diagnostics to stderr.
//  If environment variable TOKU_HUGE_PAGES_OK is set, then don't complain.
{
    char *toku_huge_pages_ok = getenv("TOKU_HUGE_PAGES_OK");
    if (toku_huge_pages_ok) {
        return false;
    } else {
        bool conf1 = check_huge_pages_config_file("/sys/kernel/mm/redhat_transparent_hugepage/enabled");
        bool conf2 = check_huge_pages_config_file("/sys/kernel/mm/transparent_hugepage/enabled");
        bool prac = check_huge_pages_in_practice();
        return conf1|conf2|prac;
    }
}
