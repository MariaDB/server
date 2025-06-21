typedef struct _IO_FILE FILE;

struct token {
	int x;
	int y;
} wat = {0, 2};

int main() {
	struct token t = {5, 7};

	wat.y = 3;

	return t.x + wat.y;
}
