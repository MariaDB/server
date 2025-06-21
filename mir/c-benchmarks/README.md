This directory contains mostly benchmarks from old [great computer
language
shootout](https://web.archive.org/web/20010124090400/http://www.bagley.org/~doug/shootout/).

I don't use benchmarks from [computer language benchmarks
game](https://benchmarksgame-team.pages.debian.net/benchmarksgame/)
because most of them depend on external libraries (gmp, pre, pthread,
etc) or io-bound and don't measure the code generation performance in
which I am actually interesting.

Some thoughts:

* `except` was considerably improved by inlining
* `matrix`, `nbody`, and `spectral-norm` can be improved by loop-invariant motion
* call-intensive bencmarkss (`funnkuch-reduce`, `method-call`, and `mandelbrot`) are slow because all calls in MIR
  are implemented through thunks permitting hot swap of function code
  * they could be improved by direct calls but it is against MIR design
