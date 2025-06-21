int printf(const char *, ...);

const void *c_vp;
void *vp;
const int *c_ip;
volatile int *v_ip;
int *ip;
const char *c_cp;

static int a = 1;

static int foo(void) {
	const void *t1 = (a) ? c_vp : c_ip;
	volatile int *t2 = (a) ? v_ip : 0;
	const volatile int *t3 = (a) ? c_ip : v_ip;
	const void *t4 = (a) ? vp : c_cp;
	const int *t5 = (a) ? ip : c_ip;
	void *t6 = (a) ? vp : ip;

	return printf("%lu, %lu, %lu\n", sizeof(*t2), sizeof(*t3), sizeof(*t5));
}

int main(void) {
	int t1 = ((1 ? -1 : 1u) < 0),
	    t2 = ((0 ? 0u : -1) < 0);

	return foo() + printf("%d, %d\n", t1, t2);
}
