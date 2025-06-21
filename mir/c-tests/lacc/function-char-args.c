#include <stdio.h>

int foo(unsigned char c, char s)
{
    return c + s + 1;
}

int main() {
    char c = -1;
    short s = -2;

    printf("%d\n", foo(c, s));
    return 1;
}
