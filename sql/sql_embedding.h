// sql/sql_embedding.h
typedef struct st_embedding_generator {
  char *name;
  char *type;
  char *provider;
  char *model_name;
  uint dimensions;
  void *private_data;
  int (*generate)(const char *input, size_t input_len, float **output, uint dimensions);
} EMBEDDING_GENERATOR;

bool init_embedding_generators();
void cleanup_embedding_generators();
void* get_embedding_generator(THD *thd, const char *name);
int register_embedding_generator(EMBEDDING_GENERATOR *generator);
int generate_embedding(void *generator, const char *input, size_t input_len, 
                      float **output, uint dimensions);
