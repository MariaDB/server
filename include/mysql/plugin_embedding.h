#ifndef MYSQL_PLUGIN_EMBEDDING_INCLUDED
#define MYSQL_PLUGIN_EMBEDDING_INCLUDED

#include "plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/*************************************************************************
  API for Embedding Generator plugins (MYSQL_EMBEDDING_PLUGIN)
*/

#define MYSQL_EMBEDDING_INTERFACE_VERSION 0x0001

/* Embedding generation modes */
enum enum_embedding_mode {
  EMBEDDING_TEXT_MODE = 0,   /* Text embedding */
  EMBEDDING_IMAGE_MODE = 1,  /* Image embedding */
  EMBEDDING_AUDIO_MODE = 2   /* Audio embedding */
};

typedef struct st_mysql_embedding_param {
  /* MySQL private data */
  void *mysql_embedding_param;
  
  /* Input parameters */
  struct charset_info_st *cs;  /* Character set info */
  char *doc;                   /* Document to embed */
  size_t length;               /* Document length */
  enum enum_embedding_mode mode; /* TEXT, IMAGE, etc. */
  
  /* Output parameters */
  int (*mysql_add_embedding)(struct st_mysql_embedding_param *param,
                            float *embedding, size_t dimensions);
  
  /* Plugin state */
  void *embedding_state;       /* Plugin private data */
  int flags;                   /* Reserved for future use */
} MYSQL_EMBEDDING_PARAM;

/* Plugin interface structure */
typedef struct st_mysql_embedding {
  int interface_version;
  
  /* Initialize plugin (if needed) */
  int (*init)(MYSQL_EMBEDDING_PARAM *param);
  
  /* Clean up resources */
  int (*deinit)(MYSQL_EMBEDDING_PARAM *param);
  
  /* Return dimensions of generated embeddings */
  size_t (*get_dimensions)(MYSQL_EMBEDDING_PARAM *param);
  
  /* Generate embedding from input */
  int (*generate)(MYSQL_EMBEDDING_PARAM *param);
} MYSQL_EMBEDDING_PLUGIN;

#ifdef __cplusplus
}
#endif

#endif /* MYSQL_PLUGIN_EMBEDDING_INCLUDED */