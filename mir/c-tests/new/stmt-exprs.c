#include <assert.h>

int f(int x) { return x; }

int div(int a, int b) {
    return f(({
        if (b == 0) return 0; // testing return inside stmt expr
        int res;
        res = a / b;
        goto skip; // testing goto inside stmt expr
        res = 0;
skip:
        res;
    }));
}

int main() {
    assert(div(6, 2) == 3);
    assert(div(1, 0) == 0);
    return 0;
}
