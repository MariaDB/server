struct typetree {
	int type;
	struct typetree *next;
};

struct var {
	struct typetree *type;
};

int main() {
	struct typetree p = {1};
	struct typetree q = {3};
	struct var root;

	p.next = &q;
	root.type = &p;

	return root.type->next->type;
}
