struct point {
	int x, y;
};

int main(void)
{
	static int arr[16];
	static struct point p;

	char foo[6] = {'a', 'b', 'c'};
	struct point q = {3};

	return p.x + arr[2] + foo[3] + q.y;
}
