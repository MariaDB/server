struct node {
	int value;
	const struct node *next;
};

int main() {
	struct node n;
	n.value = 42;
	n.next = &n;
	return n.next->value;
}
