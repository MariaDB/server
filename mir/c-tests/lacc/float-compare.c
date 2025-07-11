#include <assert.h>

int main(void) {
    float f = 5.67f, g = 5.97f;
    double d = 3.87, p = 12.9;

    do {
        assert(f < g);
        assert(p > f);
        assert(f == f);
        assert(p >= d);
    } while (p < f);

    return p >= d;
}
