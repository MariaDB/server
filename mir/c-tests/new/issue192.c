typedef struct vec2 {
  double x, y;
} vec2;

static inline vec2 vec2_add (vec2 a, vec2 b) {
  if (1) {
    return (vec2){a.x + b.y, a.y + b.y};
  } else {
    return (vec2){a.x + b.y, a.y + b.y};
  }
}

int main () { return 0; }
