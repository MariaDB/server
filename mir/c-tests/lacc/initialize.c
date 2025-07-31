typedef struct {
	int x;
	int y;
	char *name;
	char tag[3];
} point_t;

char s[] = "Hello";

point_t p = {1, 2, "Hello", {'a', 'b', 'c'}};

char tull[] = {'t', 'u', 'l', 'l', '\0'};

static char chr;
static char *str;
static char **ptrs;

int main() {
	point_t q = {1, 2, "Hello", {0, 'b', 'c'}};

	char t[] = {'h', 'e', 'l', 0, 'o', '\0'};

	int ch = (ptrs == 0) && (str == 0) && (chr == 0);

	return sizeof(p.name) + p.tag[1] + s[5] + t[0] + q.x + ch;
}
