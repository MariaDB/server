/* test for a bug for typedef and member scope */
typedef int tree;
struct {
  tree tree[1];
  tree tp[1];
} s;
int main (void) { return 0; }
