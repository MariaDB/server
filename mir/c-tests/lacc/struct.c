typedef struct {
  int x;
  int y;
  struct {
    int val;
  } z;
} point_t;

int main () {
  point_t point;
  point_t *foo;

  point.x = 1;
  point.y = 3;

  foo = &point;
  foo->x = 17;
  foo->z.val = 5;

  return (unsigned char) (point.y * point.z.val - point.x) != 254;
}
