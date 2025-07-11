#define foo(v) bar(v, a + 2)
#define bar(v, i) v = 42 + i;

int main() {
	int a = 1;
	foo(a);
	return a;
}
