

/* -*- mode: c -*-
 * $Id: lists.gcc,v 1.3 2001/04/29 04:39:50 doug Exp $
 * http://www.bagley.org/~doug/shootout/
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SIZE 10000

// a simple Double Linked List
// the head node is special, it's val is length of list
typedef struct DLL {
    int val;
    struct DLL *next;    
    struct DLL *prev;    
} DLL;

static inline int list_length(DLL *head) { return(head->val); }
static inline int list_empty(DLL *head) { return(list_length(head) == 0); }
static inline DLL *list_first(DLL *head) { return(head->next); }
static inline DLL *list_last(DLL *head) { return(head->prev); }

void list_push_tail(DLL *head, DLL *item) {
    DLL *tail = head->prev;
    tail->next = item;
    item->next = head;
    head->prev = item;
    item->prev = tail;
    head->val++;
}

DLL *list_pop_tail(DLL *head) {
    DLL *prev, *tail;
    if (list_empty(head)) return(NULL);
    tail = head->prev;
    prev = tail->prev;
    prev->next = head;
    head->prev = prev;
    head->val--;
    return(tail);
}

void list_push_head(DLL *head, DLL *item) {
    DLL *next = head->next;
    head->next = item;
    next->prev = item;
    item->next = next;
    item->prev = head;
    head->val++;
}

DLL *list_pop_head(DLL *head) {
    DLL *next;
    if (list_empty(head)) return(NULL);
    next = head->next;
    head->next = next->next;
    next->next->prev = head;
    head->val--;
    return(next);
}

int list_equal(DLL *x, DLL *y) {
    DLL *xp, *yp;
    // first val's checked will be list lengths
    for (xp=x, yp=y; xp->next != x; xp=xp->next, yp=yp->next) {
    if (xp->val != yp->val) return(0);
    }
    if (xp->val != yp->val) return(0);
    return(yp->next == y);
}

void list_print(char *msg, DLL *x) {
    DLL *xp, *first = x->next;
    int i = 0;
    printf(msg);
    printf("length: %d\n", list_length(x));
    for (xp=x->next; xp->next != first; xp=xp->next) {
    printf("i:%3d  v:%3d  n:%3d  p:%3d\n", ++i,
           xp->val, xp->next->val, xp->prev->val);
    }
    printf("[last entry points to list head]\n");
    printf("[val of next of tail is:  %d]\n", xp->next->val);
}

DLL *list_new() {
    DLL *l = (DLL *)malloc(sizeof(DLL));
    l->next = l;
    l->prev = l;
    l->val = 0;
    return(l);
}


DLL *list_sequence(int from, int to) {
    int size, tmp, i, j;
    DLL *l;
    if (from > to) {
    tmp = from; from = to; to = tmp;
    }
    size = to - from + 1;
    l = (DLL *)malloc((size+1) * sizeof(DLL));
    from--;
    for (i=0, j=1; i<size; ++i, ++j) {
    l[i].next = &l[i+1];
    l[j].prev = &l[j-1];
    l[i].val = from++;
    }
    l[0].prev = &l[size];
    l[size].next = &l[0];
    l[size].prev = &l[size-1];
    l[size].val = from;
    l[0].val = size;
    return(l);
}

DLL *list_copy(DLL *x) {
    int i, j, size = list_length(x);
    DLL *xp, *l = (DLL *)malloc((size+1) * sizeof(DLL));
    for (i=0, j=1, xp=x; i<size; i++, j++, xp=xp->next) {
    l[i].next = &l[j];
    l[j].prev = &l[i];
    l[i].val = xp->val;
    }
    l[0].prev = &l[size];
    l[size].next = &l[0];
    l[size].val = list_last(x)->val;
    return(l);
}

void list_reverse (DLL *head) {
    DLL *tmp, *p = head;
    do {
    tmp = p->next;
    p->next = p->prev;
    p->prev = tmp;
    p = tmp;
    } while (p != head);
}

int test_lists() {
    int len = 0;
    // create a list of integers (li1) from 1 to SIZE
    DLL *li1 = list_sequence(1, SIZE);
    // copy the list to li2
    DLL *li2 = list_copy(li1);
    // remove each individual item from left side of li2 and
    // append to right side of li3 (preserving order)
    DLL *li3 = list_new();
    // compare li2 and li1 for equality
    if (!list_equal(li2, li1)) {
    fprintf(stderr, "li2 and li1 are not equal\n");
    exit(1);
    }
    while (!list_empty(li2)) {
    list_push_tail(li3, list_pop_head(li2));
    }
    // li2 must now be empty
    if (!list_empty(li2)) {
    fprintf(stderr, "li2 should be empty now\n");
    exit(1);
    }
    // remove each individual item from right side of li3 and
    // append to right side of li2 (reversing list)
    while (!list_empty(li3)) {
    list_push_tail(li2, list_pop_tail(li3));
    }
    // li3 must now be empty
    if (!list_empty(li3)) {
    fprintf(stderr, "li3 should be empty now\n");
    exit(1);
    }
    // reverse li1 in place
    list_reverse(li1);
    // check that li1's first item is now SIZE
    if (list_first(li1)->val != SIZE) {
    fprintf(stderr, "li1 first value wrong, wanted %d, got %d\n",
        SIZE, list_first(li1)->val);
    exit(1);
    }
    // check that li1's last item is now 1
    if (list_last(li1)->val != 1) {
    fprintf(stderr, "last value wrong, wanted %d, got %d\n",
        SIZE, list_last(li1)->val);
    exit(1);
    }
    // check that li2's first item is now SIZE
    if (list_first(li2)->val != SIZE) {
    fprintf(stderr, "li2 first value wrong, wanted %d, got %d\n",
        SIZE, list_first(li2)->val);
    exit(1);
    }
    // check that li2's last item is now 1
    if (list_last(li2)->val != 1) {
    fprintf(stderr, "last value wrong, wanted %d, got %d\n",
        SIZE, list_last(li2)->val);
    exit(1);
    }
    // check that li1's length is still SIZE
    if (list_length(li1) != SIZE) {
    fprintf(stderr, "li1 size wrong, wanted %d, got %d\n",
        SIZE, list_length(li1));
    exit(1);
    }
    // compare li1 and li2 for equality
    if (!list_equal(li1, li2)) {
    fprintf(stderr, "li1 and li2 are not equal\n");
    exit(1);
    }
    len = list_length(li1);
    free(li1);
    free(li2);
    free(li3);
    // return the length of the list
    return(len);
}

int main(int argc, char *argv[]) {
    int n = ((argc == 2) ? atoi(argv[1]) : 1);
    int result = 0;
    while(n--) result = test_lists();
    printf("%d\n", result);
    return 0;
}
