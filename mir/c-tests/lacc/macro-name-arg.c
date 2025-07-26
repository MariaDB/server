#define ADD(a, b) a + b
#define APPLY(func, a, b) func(a, b) 
#define N APPLY(ADD, 3, 5)

int main(void) {
	return N;
}
