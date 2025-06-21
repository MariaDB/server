typedef struct Coo {
  float size;
} Coo;

typedef struct Boo {
  Coo coo[4];
} Boo;

typedef struct Aoo {
  Boo boo;
} Aoo;

void abort (void);
int main () {
  Aoo aoo = (Aoo){
    .boo = {
      .coo = {{
        .size = 42,
      }},
    },
  };
  if (aoo.boo.coo[0].size != 42) abort ();
  return 0;
}
