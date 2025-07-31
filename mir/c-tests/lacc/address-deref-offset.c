struct {
	int x, y;
} wat[] = {{1, 2}, {3, 4}};

int main(void) {
	int *p = &wat[1].y;
	return *p;
}
