/**
 * Simple Text Embedding Generator Plugin
 *
 * This is a demonstration plugin that generates deterministic embeddings
 * from text for testing and demonstration purposes. It does not use a real
 * machine learning model - it just creates vectors based on simple text hashing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "mysql/plugin.h"
#include "mysql/plugin_embedding.h"

// Plugin state
static size_t s_dimensions = 384;

/**
* Initialize the plugin
*/
static int simple_embedder_init(MYSQL_EMBEDDING_PARAM *param)
{
  // Nothing to initialize for this simple plugin
  return 0;
}

/**
* Clean up resources
*/
static int simple_embedder_deinit(MYSQL_EMBEDDING_PARAM *param)
{
  // Nothing to clean up for this simple plugin
  return 0;
}

/**
* Return the embedding dimensions
*/
static size_t simple_embedder_get_dimensions(MYSQL_EMBEDDING_PARAM *param)
{
  return s_dimensions;
}

/**
* Generate an embedding from text
* 
* This is a very simple deterministic function that creates consistent
* vectors for the same input text. It is NOT a real embedding model and
* should only be used for testing and demonstration.
*/
static int simple_embedder_generate(MYSQL_EMBEDDING_PARAM *param)
{
  size_t dim = simple_embedder_get_dimensions(param);
  float *embedding = (float*)malloc(dim * sizeof(float));
  if (!embedding)
    return 1; // Out of memory
  
  // Empty input or NULL input should produce a zero vector
  if (!param->doc || param->length == 0)
  {
    memset(embedding, 0, dim * sizeof(float));
    param->mysql_add_embedding(param, embedding, dim);
    free(embedding);
    return 0;
  }
  
  // Simple hashing function to generate a deterministic embedding
  // This is NOT a real embedding model!
  for (size_t i = 0; i < dim; i++)
  {
    float value = 0.0f;
    
    // Mix the characters with position information
    for (size_t j = 0; j < param->length; j++)
    {
      value += (param->doc[j] * (j + 1) * (i + 1)) / 255.0f;
    }
    
    // Apply some transformations to spread values in [-1, 1]
    embedding[i] = (sinf(value) + cosf(value * 1.3f)) / 2.0f;
  }
  
  // Normalize the vector (L2 norm)
  float norm = 0.0f;
  for (size_t i = 0; i < dim; i++)
  {
    norm += embedding[i] * embedding[i];
  }
  
  norm = sqrtf(norm);
  if (norm > 0.0f)
  {
    for (size_t i = 0; i < dim; i++)
    {
      embedding[i] /= norm;
    }
  }
  
  // Pass the embedding back to MariaDB
  int result = param->mysql_add_embedding(param, embedding, dim);
  
  free(embedding);
  return result;
}

// Plugin descriptor
static struct st_mysql_embedding simple_embedder_descriptor = {
  MYSQL_EMBEDDING_INTERFACE_VERSION,
  simple_embedder_generate,
  simple_embedder_init,
  simple_embedder_deinit,
  simple_embedder_get_dimensions
};

// Plugin declaration
mysql_declare_plugin(simple_embedder)
{
  MYSQL_EMBEDDING_PLUGIN,
  &simple_embedder_descriptor,
  "simple_embedder",
  "MariaDB Corporation",
  "Simple text embedding generator for testing",
  PLUGIN_LICENSE_GPL,
  NULL,                       /* Plugin initialization function */
  NULL,                       /* Plugin deinitialization function */
  0x0001,                     /* Plugin version */
  NULL,                       /* Status variables */
  NULL,                       /* System variables */
  "1.0",                      /* Version string */
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
}
mysql_declare_plugin_end;
