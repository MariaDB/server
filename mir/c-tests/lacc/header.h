#ifndef HEADER_H
#define HEADER_H

#ifndef FOO
  #define FOO 1
#endif

#undef FOO

#if !defined(FOO) && !(2 * FOO + 1 != 1)
  #define FOO 0
#endif

#define _BAR 500

#if _BAR < 500
# define _BAZ 2
#elif _BAR < 600
# define _BAZ 3
#elif _BAR < 700
# define _BAZ 4
#else
# define _BAZ 5
#endif

#if _BAZ != 3
# error Wrong!
#endif

#endif
