#define build(code, ...) _buildN1 (build, _buildC1 (__VA_ARGS__)) (code, __VA_ARGS__)
#define _buildN1(BASE, X) _buildN2 (BASE, X)
#define _buildN2(BASE, X) BASE##X
#define _buildC1(...) _buildC2 (__VA_ARGS__, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 0)
#define _buildC2(x, a1, a2, a3, a4, a5, a6, a7, a8, a9, c, ...) c

int build1_stat (int a, double d, int b) { return a; }
#define build1(a, b, c) build1_stat (a, b, c)

int main (void) { return build (0, 1.0, 2); }
