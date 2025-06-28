struct obj {
	int mem;
	union either {
		signed char i;
		unsigned int u;
	} d;
};

struct obj data = {1};

int main(void) {
	return 0;
}
