#define triple(a) (3 * (a))
#define FOO 2

#if (FOO ? 0 : 1)
# error wrong
#endif
#if BAR == 0 && defined FOO && triple(1) == 3

int main() {
	return 1;
}

#elif triple(0)
#error Not possible
#endif

#define T1 ((1 ? -1 : (0, 0u)) < 0)

#if T1
# error Should be false
#endif

