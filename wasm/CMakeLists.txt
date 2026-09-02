# WASM glue executable: libmariadbd + JS module.

SET(WASM_DIST ${CMAKE_CURRENT_SOURCE_DIR}/dist)
FILE(MAKE_DIRECTORY ${WASM_DIST})

ADD_EXECUTABLE(mariadblite mariadblite.c)
TARGET_COMPILE_DEFINITIONS(mariadblite PRIVATE EMBEDDED_LIBRARY)
TARGET_INCLUDE_DIRECTORIES(mariadblite PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_BINARY_DIR}/include
)
TARGET_LINK_LIBRARIES(mariadblite PRIVATE mysqlserver)
ADD_DEPENDENCIES(mariadblite GenError)

SET_TARGET_PROPERTIES(mariadblite PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${WASM_DIST}
  SUFFIX ".js"
)

SET(WASM_EXPORTED_FUNCTIONS
  "['_mdl_open','_mdl_query','_mdl_exec_multi','_mdl_escape','_mdl_close','_mdl_free','_malloc','_free']")
SET(WASM_EXPORTED_RUNTIME
  "['ccall','cwrap','UTF8ToString','stringToUTF8','lengthBytesUTF8','FS']")

TARGET_LINK_OPTIONS(mariadblite PRIVATE
  -O2
  -pthread
  -fexceptions
  "SHELL:-s MODULARIZE=1"
  "SHELL:-s EXPORT_ES6=1"
  "SHELL:-s EXPORT_NAME=createMariaDBlite"
  "SHELL:-s ENVIRONMENT=web,worker,node"
  "SHELL:-s ALLOW_MEMORY_GROWTH=1"
  "SHELL:-s INITIAL_MEMORY=268435456"
  "SHELL:-s MAXIMUM_MEMORY=2147483648"
  "SHELL:-s STACK_SIZE=2097152"
  "SHELL:-s PTHREAD_POOL_SIZE=16"
  "SHELL:-s PTHREAD_POOL_SIZE_STRICT=0"
  "SHELL:-s NO_EXIT_RUNTIME=1"
  "SHELL:-s INVOKE_RUN=0"
  "SHELL:-s EXIT_RUNTIME=0"
  "SHELL:-s WASM_BIGINT=1"
  "SHELL:-s DISABLE_EXCEPTION_CATCHING=0"
  "SHELL:-s ALLOW_TABLE_GROWTH=1"
  "SHELL:-s EXPORTED_FUNCTIONS=${WASM_EXPORTED_FUNCTIONS}"
  "SHELL:-s EXPORTED_RUNTIME_METHODS=${WASM_EXPORTED_RUNTIME}"
  "SHELL:-lnodefs.js"
  "SHELL:-lidbfs.js"
  "SHELL:--embed-file ${CMAKE_SOURCE_DIR}/sql/share/charsets@/mariadb/share/charsets"
  "SHELL:--embed-file ${CMAKE_BINARY_DIR}/sql/share@/mariadb/share"
)

ADD_CUSTOM_COMMAND(TARGET mariadblite POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_CURRENT_SOURCE_DIR}/src/index.mjs
    ${CMAKE_CURRENT_SOURCE_DIR}/src/worker.mjs
    ${CMAKE_CURRENT_SOURCE_DIR}/src/worker-entry.mjs
    ${WASM_DIST}/
  COMMENT "Install JS package entries next to the WASM module"
)
