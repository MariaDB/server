// sql/sql_embedding.cc
/*
Add:
    1. System table creation for storing generator definitions
    2. Implementation of CREATE EMBEDDING GENERATOR command
    3. Registry to track available embedding generators
    4. Cache for frequently used embeddings
*/

static HASH embedding_generators;
static mysql_rwlock_t generators_rwlock;

bool init_embedding_generators()
{
  if (my_hash_init(&embedding_generators, system_charset_info, 32, 0, 0, 
                  (my_hash_get_key)get_generator_key, 0, 0, PSI_INSTRUMENT_ME))
    return true;
    
  mysql_rwlock_init(key_rwlock_embedding_generators, &generators_rwlock);
  return false;
}

void* get_embedding_generator(THD *thd, const char *name)
{
  EMBEDDING_GENERATOR *generator;
  
  mysql_rwlock_rdlock(&generators_rwlock);
  generator = (EMBEDDING_GENERATOR*)my_hash_search(&embedding_generators, 
                                                  (uchar*)name, strlen(name));
  mysql_rwlock_unlock(&generators_rwlock);
  
  return generator;
}

int register_embedding_generator(EMBEDDING_GENERATOR *generator)
{
  mysql_rwlock_wrlock(&generators_rwlock);
  int result = my_hash_insert(&embedding_generators, (uchar*)generator);
  mysql_rwlock_unlock(&generators_rwlock);
  
  return result;
}

int generate_embedding(void *generator_ptr, const char *input, size_t input_len, 
                      float **output, uint dimensions)
{
  EMBEDDING_GENERATOR *generator = (EMBEDDING_GENERATOR*)generator_ptr;
  return generator->generate(input, input_len, output, dimensions);
}


void cleanup_embedding_generators()
{
  uint i;
  EMBEDDING_GENERATOR *generator;
  
  mysql_rwlock_wrlock(&generators_rwlock);
  for (i = 0; i < embedding_generators.records; i++)
  {
    generator = (EMBEDDING_GENERATOR*)my_hash_element(&embedding_generators, i);
    if (generator)
    {
      my_free(generator->name);
      my_free(generator->type);
      my_free(generator->provider);
      my_free(generator->model_name);
      // Free any private data if needed
    }
  }
  my_hash_free(&embedding_generators);
  mysql_rwlock_unlock(&generators_rwlock);
  mysql_rwlock_destroy(&generators_rwlock);
}
