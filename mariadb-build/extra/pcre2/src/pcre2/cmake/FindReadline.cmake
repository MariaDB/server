# from http://websvn.kde.org/trunk/KDE/kdeedu/cmake/modules/FindReadline.cmake
# http://websvn.kde.org/trunk/KDE/kdeedu/cmake/modules/COPYING-CMAKE-SCRIPTS
# --> BSD licensed
#
# GNU Readline library finder

find_path(READLINE_INCLUDE_DIR readline/readline.h PATH_SUFFIXES include)
mark_as_advanced(READLINE_INCLUDE_DIR)

find_library(READLINE_LIBRARY NAMES readline)
mark_as_advanced(READLINE_LIBRARY)

if(READLINE_INCLUDE_DIR AND READLINE_LIBRARY)
  # Check if we need to link to ncurses as well

  include(CheckSymbolExists)
  include(CMakePushCheckState)

  cmake_push_check_state(RESET)
  set(CMAKE_REQUIRED_LIBRARIES "${READLINE_LIBRARY}")
  set(CMAKE_REQUIRED_INCLUDES "${READLINE_INCLUDE_DIR}")
  check_symbol_exists("readline" "stdio.h;readline/readline.h" HAVE_READLINE_FUNC)

  if(NOT HAVE_READLINE_FUNC)
    foreach(
      lib
      IN
      ITEMS tinfo curses ncurses ncursesw termcap
    )
      find_library(NCURSES_LIBRARY_${lib} NAMES ${lib})
      mark_as_advanced(NCURSES_LIBRARY_${lib})
      if(NCURSES_LIBRARY_${lib})
        cmake_reset_check_state()
        set(CMAKE_REQUIRED_LIBRARIES "${READLINE_LIBRARY}" "${NCURSES_LIBRARY_${lib}}")
        set(CMAKE_REQUIRED_INCLUDES "${READLINE_INCLUDE_DIR}")
        check_symbol_exists("readline" "stdio.h;readline/readline.h" HAVE_READLINE_FUNC_${lib})

        if(HAVE_READLINE_FUNC_${lib})
          message(STATUS "Looking for readline - readline needs ${lib}")
          set(NCURSES_LIBRARY "${NCURSES_LIBRARY_${lib}}" CACHE FILEPATH "Path to the ncurses library")
          mark_as_advanced(NCURSES_LIBRARY)
          break()
        endif()
      endif()
    endforeach()
  endif()

  cmake_pop_check_state()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Readline DEFAULT_MSG READLINE_LIBRARY READLINE_INCLUDE_DIR)
