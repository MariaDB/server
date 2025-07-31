int printf(const char *, ...);

#define PRINT_RAW(obj) print_raw_bytes((char *) &(obj), sizeof(obj))

static void print_raw_bytes(char *data, unsigned long size)
{
	int i;
	for (i = 0; i < size; ++i)
		printf("%d, ", data[i]);
	printf("\n");
}

struct S1 {
	short a : 4;
	int b : 15;
} s1_0 = {0},
  s1_1 = {1, 5};

static void test_S1(struct S1 p) {
	struct S1 q = {8, -2};
	int a = p.a, b = p.b;
	q.b = a + b;
	PRINT_RAW(p);
	PRINT_RAW(q);
	printf("S1: {%d, %d} (%lu)\n", p.a, p.b, sizeof(q));
}

struct S2 {
	char a : 7;
} s2_0 = {0},
  s2_1 = {-1};

static void test_S2(struct S2 p) {
	struct S2 q = {8};
	int a = p.a;
	q.a = a + 3;
	PRINT_RAW(p);
	PRINT_RAW(q);
	printf("S2: {%d} (%lu)\n", p.a, sizeof(q));
}

struct S3 {
	short a : 7;
} s3_0 = {0},
  s3_1 = {-2};

struct S4 {
	int a : 7;
	short b : 16;
} s4_0 = {0},
  s4_1 = {-1, 2};

struct S5 {
	int a : 20;
	long b : 30;
} s5_0 = {0},
  s5_1 = {-1, 8976876L};

struct S6 {
	short a : 14;
	short b : 15;
} s6_0 = {0},
  s6_1 = {9862, 12438};

struct S7 {
	char a : 5;
	char b : 4;
	char c : 5;
	char d : 4;
} s7_0 = {0},
  s7_1 = {25, 8, 30, 7};

static void test_S7(struct S7 p) {
	struct S7 q = {8, -2};
	int a = p.a, b = p.b;
	q.b = a + b;
	PRINT_RAW(p);
	PRINT_RAW(q);
	printf("S7: {%d, %d, %d, %d} (%lu)\n", p.a, p.b, p.c, p.d, sizeof(q));
}

struct S8 {
	short a : 3;
	char b : 5;
	char c : 3;
} s8_0 = {0},
  s8_1 = {2, 7, 1};

struct S9 {
	char a : 4;
	long b : 4;
} s9_0 = {0},
  s9_1 = {3, 4};

struct SA {
	char a : 4;
	long b : 4;
	int c : 10;
} sa_0 = {0},
  sa_1 = {3, 2, 18};

static void test_SA(struct SA p) {
	struct SA q = {8, -2L};
	int a = p.a, b = p.c;
	q.b = a;
	q.a = q.b;
	PRINT_RAW(p);
	PRINT_RAW(q);
	printf("SA: {%d, %d, %d} (%lu)\n", p.a, p.b, p.c, sizeof(q));
}

struct SB {
	char a : 4;
	long : 0;
	char b : 4;
} sb_0 = {0},
  sb_1 = {2, 3};

struct SC {
	short a : 4;
	char : 0;
	char b : 4;
} sc_0 = {0},
  sc_1 = {1, -1};

struct SD {
	int a : 28;
	char : 0;
	char b : 4;
} sd_0 = {0},
  sd_1 = {-1, -2};

int main(void) {
	test_S1(s1_0);
	test_S1(s1_1);

	test_S2(s2_0);
	test_S2(s2_1);

	test_S7(s7_0);
	test_S7(s7_1);

	test_SA(sa_0);
	test_SA(sa_1);

	return printf("S1: %lu\n", sizeof(struct S1)),
		+  printf("S2: %lu\n", sizeof(struct S2)),
		+  printf("S3: %lu\n", sizeof(struct S3)),
		+  printf("S4: %lu\n", sizeof(struct S4)),
		+  printf("S5: %lu\n", sizeof(struct S5)),
		+  printf("S6: %lu\n", sizeof(struct S6)),
		+  printf("S7: %lu\n", sizeof(struct S7)),
		+  printf("S8: %lu\n", sizeof(struct S8)),
		+  printf("S9: %lu\n", sizeof(struct S9)),
		+  printf("SA: %lu\n", sizeof(struct SA)),
		+  printf("SB: %lu\n", sizeof(struct SB)),
		+  printf("SC: %lu\n", sizeof(struct SC)),
		+  printf("SD: %lu\n", sizeof(struct SD));
}
