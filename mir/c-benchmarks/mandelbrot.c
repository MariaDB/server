#include <stdlib.h>
#include <stdio.h>

int main (int argc, char **argv) {
  int w, h, bit_num = 0;
  char byte_acc = 0;
  int i, iter = 50;
  double x, y, limit = 2.0;
  double Zr, Zi, Cr, Ci, Tr, Ti;

  w = h = atoi (argv[1]);

  printf ("P4\n%d %d\n", w, h);

  for (y = 0; y < h; ++y) {
    for (x = 0; x < w; ++x) {
      Zr = Zi = Tr = Ti = 0.0;
      Cr = (2.0 * x / w - 1.5);
      Ci = (2.0 * y / h - 1.0);

      for (i = 0; i < iter && (Tr + Ti <= limit * limit); ++i) {
        Zi = 2.0 * Zr * Zi + Ci;
        Zr = Tr - Ti + Cr;
        Tr = Zr * Zr;
        Ti = Zi * Zi;
      }

      byte_acc <<= 1;
      if (Tr + Ti <= limit * limit) byte_acc |= 0x01;

      ++bit_num;

      if (bit_num == 8) {
        putc (byte_acc, stdout);
        byte_acc = 0;
        bit_num = 0;
      } else if (x == w - 1) {
        byte_acc <<= (8 - w % 8);
        putc (byte_acc, stdout);
        byte_acc = 0;
        bit_num = 0;
      }
    }
  }
}
