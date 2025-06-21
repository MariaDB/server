typedef struct {
  int i;
} value;
enum tree_code { CALL_EXPR };
struct tree_common {
  int code;
};
union tree_node {
  int code;
};
typedef union tree_node *tree;

static int add_pointed_to_expr (tree value) { return ((value)->code) == CALL_EXPR; }
static int add_pointed_to_expr2 (void) {
  tree value;
  return ((value)->code) == CALL_EXPR;
}

int main (void) { return 0; }
