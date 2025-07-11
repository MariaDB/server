int i;
struct s {
  int i;
} s;
int f (int n) { return (n == 0 ? &s : (void *) 0)->i; }
int main (void) { return f (i); }
