/* a test for bug in type sharing and type modification in cond-expr */
typedef union tree_node *tree;
union tree_node {
  int *chain;
};
struct basic_block_def {
  tree stmt_list;
};
typedef struct basic_block_def *basic_block;
static tree first;
static void create_bb (void *h, void *e, basic_block bb) { bb->stmt_list = h ? h : first; }
static int f (tree first) { return first->chain == 0; }
int main (void) { return 0; }
