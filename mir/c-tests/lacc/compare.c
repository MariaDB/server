int printf(const char *, ...);

int arr[] = {1, 2, 3};

void *p = &arr[0];
void *q = &arr[1];
const int *r = &arr[2];

int main(void) {
	return printf("%d, %d, %d, %d\n", p < q, p > q, p == r, r != q);
}
