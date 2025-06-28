struct point {
	long x, y;
};

struct point p;

int main() {
	struct point q = {1, 2};

	p = q;
	return p.y;
}
