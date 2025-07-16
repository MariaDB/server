#ifdef NO
# error not supposed to happen
#elif 0
# error "not this either"
#
#else
# include "header.h"
# define triple(a) (3 * (a))
#endif

#pragma

#if 1
# pragma = This should be ignored
#endif

#     
# 	 	
#

#define iscool() 1;
#	
#include "header.h"

int main() {
	return FOO + triple(3) + iscool ( );
}
