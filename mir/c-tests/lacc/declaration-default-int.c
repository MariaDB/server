static baz = 42;

bar(a, b) {
	return a + b + baz;
}

static foo(a, b)
long b;
{
	auto c = a + b;
	return bar(c, a * a);
}

main() {
	register m;
	m = foo(2, 3);
	return m;
}
