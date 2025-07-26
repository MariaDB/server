struct point {
	char x;
	int y;
	void *next;
};

struct mem {
	int arr[255];
};

int func(struct point p, struct mem m, long a, long b, long c, long d, int e) {
	p.x = 1;
	m.arr[0] = 1;
	return p.y + m.arr[0] + a + b + c + d + e;
}

int main () {
	struct point p = {1, 2, 0};
	struct mem arr;
	return func(p, arr, 1, 2, 3, 4, 5);
}
