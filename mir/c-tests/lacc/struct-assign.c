struct {
	short s;
} p1 = {1}, p2 = {2};

struct {
	char a;
	short b, c;
} q1 = {3}, q2 = {4};

int main(void) {
	p1 = p2;
	q1 = q2;
	return p1.s + q1.a + q1.b + q1.c;
}
