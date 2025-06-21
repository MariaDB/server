int printf(const char *, ...);

/* "Duff's Device", adapted from 
 * https://groups.google.com/forum/#!msg/net.lang.c/3KFq-67DzdQ/TKl64DiBAGYJ
 */
void send(short *to, short *from, int count) {
    int n = (count + 7)/8;
    switch (count % 8) {
    case 0: do  {   *to += *from++;
    case 7:         *to += *from++;
    case 6:         *to += *from++;
    case 5:         *to += *from++;
    case 4:         *to += *from++;
    case 3:         *to += *from++;
    case 2:         *to += *from++;
    case 1:         *to += *from++;
            } while (--n > 0);
    }
}

static int result(short *to, short *from, int count) {
    int i;
    for (i = 0; i < count; ++i) {
        printf("(%d, %d)", to[i], from[i]);
        if (i < count - 1) {
            printf(", ");
        }
    }
    printf("\n");
    return 0;
}

static short p[16] = {1, 2, 3, 4, 5, 6, 7, 8, 3, 8, 5, 1, 0, 0, 9, 2};
static short q[16] = {4, 2, 7, 2, 1, 2, 5, 7, 2, 6, 8, 0, 4, 2, 6, 3};

int main(void) {
    send(p + 2, q, 16);
    send(p + 5, q, 13);
    send(p + 7, p, 5);

    return result(p, q, 16);
}
