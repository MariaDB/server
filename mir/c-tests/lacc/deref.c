struct point {
	int x, y;
};

struct object {
	int ival;
	char cval;
	struct point *pt;
};

int main() {
	struct point pt = {4, 5};
	struct object obj = {1, 'a'};

	obj.pt = &pt;
	obj.pt->x = 3;
	obj.pt->y = 4;

	return pt.x + pt.y;
}
