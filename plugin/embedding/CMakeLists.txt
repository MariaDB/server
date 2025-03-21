SET(SIMPLE_EMBEDDER_SOURCES
  simple_embedder/simple_embedder.c
)

MYSQL_ADD_PLUGIN(simple_embedder ${SIMPLE_EMBEDDER_SOURCES}
  MODULE_ONLY)