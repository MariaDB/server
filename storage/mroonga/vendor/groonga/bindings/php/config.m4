
PHP_ARG_WITH(groonga, whether groonga is available,[  --with-groonga[=DIR] With groonga support])


if test "$PHP_GROONGA" != "no"; then


  if test -r "$PHP_GROONGA/include/groonga.h"; then
	PHP_GROONGA_DIR="$PHP_GROONGA"
	PHP_ADD_INCLUDE($PHP_GROONGA_DIR/include)
  elif test -r "$PHP_GROONGA/include/groonga/groonga.h"; then
	PHP_GROONGA_DIR="$PHP_GROONGA"
	PHP_ADD_INCLUDE($PHP_GROONGA_DIR/include/groonga)
  else
	AC_MSG_CHECKING(for groonga in default path)
	for i in /usr /usr/local; do
	  if test -r "$i/include/groonga/groonga.h"; then
		PHP_GROONGA_DIR=$i
		AC_MSG_RESULT(found in $i)
		break
	  fi
	done
	if test "x" = "x$PHP_GROONGA_DIR"; then
	  AC_MSG_ERROR(not found)
	fi
	PHP_ADD_INCLUDE($PHP_GROONGA_DIR/include)
  fi

  export OLD_CPPFLAGS="$CPPFLAGS"
  export CPPFLAGS="$CPPFLAGS $INCLUDES -DHAVE_GROONGA"
  AC_CHECK_HEADER([groonga.h], [], AC_MSG_ERROR('groonga.h' header not found))
  PHP_SUBST(GROONGA_SHARED_LIBADD)


  PHP_CHECK_LIBRARY(groonga, grn_init,
  [
	PHP_ADD_LIBRARY_WITH_PATH(groonga, $PHP_GROONGA_DIR/lib, GROONGA_SHARED_LIBADD)
  ],[
	AC_MSG_ERROR([wrong groonga lib version or lib not found])
  ],[
	-L$PHP_GROONGA_DIR/lib
  ])
  export CPPFLAGS="$OLD_CPPFLAGS"

  export OLD_CPPFLAGS="$CPPFLAGS"
  export CPPFLAGS="$CPPFLAGS $INCLUDES -DHAVE_GROONGA"

  AC_MSG_CHECKING(PHP version)
  AC_TRY_COMPILE([#include <php_version.h>], [
#if PHP_VERSION_ID < 40000
#error  this extension requires at least PHP version 4.0.0
#endif
],
[AC_MSG_RESULT(ok)],
[AC_MSG_ERROR([need at least PHP 4.0.0])])

  export CPPFLAGS="$OLD_CPPFLAGS"


  PHP_SUBST(GROONGA_SHARED_LIBADD)
  AC_DEFINE(HAVE_GROONGA, 1, [ ])

  PHP_NEW_EXTENSION(groonga, groonga.c , $ext_shared)

fi

