#define X(v, const)  ((v) * (const))

int main(void) {
	int a = 2;
	return X(a, 4);
}
