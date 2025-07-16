

/* -*- mode: c -*-
 * $Id: heapsort.gcc,v 1.10 2001/05/08 02:46:59 doug Exp $
 * http://www.bagley.org/~doug/shootout/
 */

#include <stdlib.h>
#include <math.h>
#include <stdio.h>

#define IM 139968
#define IA   3877
#define IC  29573

double
gen_random(double max) {
    static long last = 42;
    return( max * (last = (last * IA + IC) % IM) / IM );
}

void
heap_sort(int n, double *ra) {
    int i, j;
    int ir = n;
    int l = (n >> 1) + 1;
    double rra;

    for (;;) {
    if (l > 1) {
        rra = ra[--l];
    } else {
        rra = ra[ir];
        ra[ir] = ra[1];
        if (--ir == 1) {
        ra[1] = rra;
        return;
        }
    }
    i = l;
    j = l << 1;
    while (j <= ir) {
        if (j < ir && ra[j] < ra[j+1]) { ++j; }
        if (rra < ra[j]) {
        ra[i] = ra[j];
        j += (i = j);
        } else {
        j = ir + 1;
        }
    }
    ra[i] = rra;
    }
}

int
main(int argc, char *argv[]) {
    int N = ((argc == 2) ? atoi(argv[1]) : 1);
    double *ary;
    int i;
    
    
    ary = (double *)malloc((N+1) * sizeof(double));
    for (i=1; i<=N; i++) {
    ary[i] = gen_random(1);
    }

    heap_sort(N, ary);

    printf("%.10g\n", ary[N]);

    free(ary);
    return(0);
}
