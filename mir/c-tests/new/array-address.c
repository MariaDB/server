typedef long HARD_REG_SET[2];
HARD_REG_SET label_live[1];
int main (void) {
  HARD_REG_SET *n = &(label_live[0]);
  return 0;
}
