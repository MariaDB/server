if(DEFINED JAVA_AWT_LIBRARY)
  return()
endif()

set(orig_CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH})
unset(CMAKE_MODULE_PATH)
include(FindJNI)
set(CMAKE_MODULE_PATH ${orig_CMAKE_MODULE_PATH})
