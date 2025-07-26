struct {
	int x, y;
} wat = {1, 2};

int main(void) {
	int p = *(&wat.y);
	return p;
}
