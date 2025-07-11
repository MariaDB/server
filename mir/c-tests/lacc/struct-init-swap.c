/* From Listing 3 in "Test-Case Reduction for C Compiler Bugs" paper, available
 * at http://www.cs.utah.edu/~regehr/papers/pldi12-preprint.pdf.
 */

int printf (const char *, ...);
struct {
	int f0;
	int f1;
	int f2;
} a, b = { 0, 0, 1 };

void fn1 () {
	a = b;
	a = a;
}

int main () {
	fn1 ();
	printf ("%d\n", a.f2);
	return 0;
}
