/* 
 * Example adapted from System V ABI, Figure 3.6.
 */

typedef struct {
	int a, b;
} structparm;

int func(int e, int f, structparm s, int g, int h, int i, int j, int k)
{
	return e + f + s.a + s.b + g + h + i + j + k;
}

int main () {
	structparm s = {8, 9};
	int e = 1, f = 2, g = 3, h = 4, i = 5, j = 6, k = 7;
	return func(e, f, s, g, h, i, j, k);
}
