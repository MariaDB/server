include_directories(${CMAKE_CURRENT_SOURCE_DIR})

set(snappy_srcs
    snappy
    snappy-c
    snappy-sinksource
    snappy-stubs-internal
    )

add_definitions("-Wno-sign-compare -Wno-unused-function -Wno-unused-parameter -fvisibility=hidden -fPIC")

## make the library, it's going to go into libtokudb.so so it needs
## to be PIC
add_library(snappy STATIC ${snappy_srcs})
set_target_properties(snappy PROPERTIES POSITION_INDEPENDENT_CODE ON)
install(
  TARGETS snappy
  DESTINATION lib
  )
install(
  FILES snappy.h snappy-stubs-public.h
  DESTINATION include
  )
