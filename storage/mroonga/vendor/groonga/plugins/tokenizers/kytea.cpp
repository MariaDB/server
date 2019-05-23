/* -*- c-basic-offset: 2 -*- */
/* Copyright(C) 2012 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#ifdef GRN_EMBEDDED
#  define GRN_PLUGIN_FUNCTION_TAG tokenizers_kytea
#endif

#include <groonga/tokenizer.h>

#include <kytea/kytea.h>
#include <kytea/string-util.h>

#include <string.h>

#include <string>
#include <vector>

namespace {

grn_plugin_mutex *kytea_mutex = NULL;
kytea::KyteaConfig *kytea_config = NULL;
kytea::Kytea *kytea_tagger = NULL;
kytea::StringUtil *kytea_util = NULL;

void kytea_init(grn_ctx *ctx);
void kytea_fin(grn_ctx *ctx);

void kytea_init(grn_ctx *ctx) {
  if (kytea_mutex || kytea_config || kytea_tagger || kytea_util) {
    GRN_PLUGIN_ERROR(ctx, GRN_TOKENIZER_ERROR,
                     "[tokenizer][kytea] "
                     "TokenKytea is already initialized");
    return;
  }

  kytea_mutex = grn_plugin_mutex_open(ctx);
  if (!kytea_mutex) {
    kytea_fin(ctx);
    GRN_PLUGIN_ERROR(ctx, GRN_NO_MEMORY_AVAILABLE,
                     "[tokenizer][kytea] "
                     "grn_plugin_mutex_open() failed");
    return;
  }

  kytea::KyteaConfig * const config = static_cast<kytea::KyteaConfig *>(
      GRN_PLUGIN_MALLOC(ctx, sizeof(kytea::KyteaConfig)));
  if (!config) {
    kytea_fin(ctx);
    GRN_PLUGIN_ERROR(ctx, GRN_NO_MEMORY_AVAILABLE,
                     "[tokenizer][kytea] "
                     "memory allocation to kytea::KyteaConfig failed");
    return;
  }

  try {
    new (config) kytea::KyteaConfig;
    kytea_config = config;
    try {
      kytea_config->setDebug(0);
      kytea_config->setOnTraining(false);
      kytea_config->parseRunCommandLine(0, NULL);
    } catch (...) {
      kytea_fin(ctx);
      GRN_PLUGIN_ERROR(ctx, GRN_TOKENIZER_ERROR,
                       "[tokenizer][kytea] "
                       "kytea::KyteaConfig settings failed");
      return;
    }
  } catch (...) {
    GRN_PLUGIN_FREE(ctx, config);
    kytea_fin(ctx);
    GRN_PLUGIN_ERROR(ctx, GRN_TOKENIZER_ERROR,
                     "[tokenizer][kytea] "
                     "kytea::KyteaConfig initialization failed");
    return;
  }

  kytea::Kytea * const tagger = static_cast<kytea::Kytea *>(
      GRN_PLUGIN_MALLOC(ctx, sizeof(kytea::Kytea)));
  if (!tagger) {
    kytea_fin(ctx);
    GRN_PLUGIN_ERROR(ctx, GRN_NO_MEMORY_AVAILABLE,
                     "[tokenizer][kytea] "
                     "memory allocation to kytea::Kytea failed");
    return;
  }

  try {
    new (tagger) kytea::Kytea;
    kytea_tagger = tagger;
    try {
      kytea_tagger->readModel(kytea_config->getModelFile().c_str());
    } catch (...) {
      kytea_fin(ctx);
      GRN_PLUGIN_ERROR(ctx, GRN_TOKENIZER_ERROR,
                       "[tokenizer][kytea] "
                       "kytea::Kytea::readModel() failed");
      return;
    }
  } catch (...) {
    GRN_PLUGIN_FREE(ctx, tagger);
    kytea_fin(ctx);
    GRN_PLUGIN_ERROR(ctx, GRN_TOKENIZER_ERROR,
                     "[tokenizer][kytea] "
                     "kytea::Kytea initialization failed");
    return;
  }

  try {
    kytea_util = kytea_tagger->getStringUtil();
  } catch (...) {
    kytea_fin(ctx);
    GRN_PLUGIN_ERROR(ctx, GRN_TOKENIZER_ERROR,
                     "[tokenizer][kytea] "
                     "kytea::Kytea::getStringUtil() failed");
    return;
  }
}

void kytea_fin(grn_ctx *ctx) {
  kytea_util = NULL;

  if (kytea_tagger) {
    kytea_tagger->~Kytea();
    GRN_PLUGIN_FREE(ctx, kytea_tagger);
    kytea_tagger = NULL;
  }

  if (kytea_config) {
    kytea_config->~KyteaConfig();
    GRN_PLUGIN_FREE(ctx, kytea_config);
    kytea_config = NULL;
  }

  if (kytea_mutex) {
    grn_plugin_mutex_close(ctx, kytea_mutex);
    kytea_mutex = NULL;
  }
}

struct grn_tokenizer_kytea {
  grn_tokenizer_query *query;
  kytea::KyteaSentence sentence;
  std::vector<std::string> tokens;
  std::size_t id;
  grn_tokenizer_token token;
  const char *rest_query_string;
  unsigned int rest_query_string_length;

  grn_tokenizer_kytea() :
    query(NULL),
    sentence(),
    tokens(),
    id(0),
    token(),
    rest_query_string(NULL)
  {
  }
  ~grn_tokenizer_kytea() {}
};

void grn_tokenizer_kytea_init(grn_ctx *ctx, grn_tokenizer_kytea *tokenizer) {
  new (tokenizer) grn_tokenizer_kytea;
  grn_tokenizer_token_init(ctx, &tokenizer->token);
}

void grn_tokenizer_kytea_fin(grn_ctx *ctx, grn_tokenizer_kytea *tokenizer) {
  grn_tokenizer_token_fin(ctx, &tokenizer->token);
  if (tokenizer->query) {
    grn_tokenizer_query_close(ctx, tokenizer->query);
  }
  tokenizer->~grn_tokenizer_kytea();
}

grn_obj *grn_kytea_init(grn_ctx *ctx, int num_args, grn_obj **args,
                        grn_user_data *user_data) {
  unsigned int normalizer_flags = 0;
  grn_tokenizer_query * const query =
      grn_tokenizer_query_open(ctx, num_args, args, normalizer_flags);
  if (!query) {
    return NULL;
  }

  grn_tokenizer_kytea * const tokenizer = static_cast<grn_tokenizer_kytea *>(
      GRN_PLUGIN_MALLOC(ctx, sizeof(grn_tokenizer_kytea)));
  if (!tokenizer) {
    grn_tokenizer_query_close(ctx, query);
    GRN_PLUGIN_ERROR(ctx, GRN_NO_MEMORY_AVAILABLE,
                     "[tokenizer][kytea] "
                     "memory allocation to grn_tokenizer_kytea failed");
    return NULL;
  }

  try {
    grn_tokenizer_kytea_init(ctx, tokenizer);
  } catch (...) {
    grn_tokenizer_query_close(ctx, query);
    GRN_PLUGIN_ERROR(ctx, GRN_TOKENIZER_ERROR,
                     "[tokenizer][kytea] "
                     "tokenizer initialization failed");
    return NULL;
  }

  tokenizer->query = query;

  grn_obj *normalized_query = query->normalized_query;
  const char *normalized_string;
  unsigned int normalized_string_length;
  grn_string_get_normalized(ctx,
                            normalized_query,
                            &normalized_string,
                            &normalized_string_length,
                            NULL);
  if (tokenizer->query->have_tokenized_delimiter) {
    tokenizer->rest_query_string = normalized_string;
    tokenizer->rest_query_string_length = normalized_string_length;
  } else {
    grn_plugin_mutex_lock(ctx, kytea_mutex);
    try {
      const std::string str(normalized_string, normalized_string_length);
      const kytea::KyteaString &surface_str = kytea_util->mapString(str);
      const kytea::KyteaString &normalized_str = kytea_util->normalize(surface_str);
      tokenizer->sentence = kytea::KyteaSentence(surface_str, normalized_str);
      kytea_tagger->calculateWS(tokenizer->sentence);
    } catch (...) {
      grn_plugin_mutex_unlock(ctx, kytea_mutex);
      GRN_PLUGIN_ERROR(ctx, GRN_TOKENIZER_ERROR,
                       "[tokenizer][kytea] "
                       "tokenization failed");
      return NULL;
    }
    grn_plugin_mutex_unlock(ctx, kytea_mutex);

    try {
      for (std::size_t i = 0; i < tokenizer->sentence.words.size(); ++i) {
        const std::string &token =
            kytea_util->showString(tokenizer->sentence.words[i].surface);
        const char *ptr = token.c_str();
        unsigned int left = static_cast<unsigned int>(token.length());
        while (left > 0) {
          const int char_length =
              grn_tokenizer_charlen(ctx, ptr, left, query->encoding);
          if ((char_length == 0) ||
              (grn_tokenizer_isspace(ctx, ptr, left, query->encoding) != 0)) {
            break;
          }
          ptr += char_length;
          left -= char_length;
        }
        if (left == 0) {
          tokenizer->tokens.push_back(token);
        }
      }
    } catch (...) {
      GRN_PLUGIN_ERROR(ctx, GRN_TOKENIZER_ERROR,
                       "[tokenizer][kytea] "
                       "adjustment failed");
      return NULL;
    }
  }

  user_data->ptr = tokenizer;
  return NULL;
}

grn_obj *grn_kytea_next(grn_ctx *ctx, int num_args, grn_obj **args,
                        grn_user_data *user_data) {
  grn_tokenizer_kytea * const tokenizer =
      static_cast<grn_tokenizer_kytea *>(user_data->ptr);

  if (tokenizer->query->have_tokenized_delimiter) {
    unsigned int rest_query_string_length =
      tokenizer->rest_query_string_length;
    const char *rest_query_string =
      grn_tokenizer_tokenized_delimiter_next(ctx,
                                             &(tokenizer->token),
                                             tokenizer->rest_query_string,
                                             rest_query_string_length,
                                             tokenizer->query->encoding);
    if (rest_query_string) {
      tokenizer->rest_query_string_length -=
        rest_query_string - tokenizer->rest_query_string;
    }
    tokenizer->rest_query_string = rest_query_string;
  } else {
    const grn_tokenizer_status status =
        ((tokenizer->id + 1) < tokenizer->tokens.size()) ?
            GRN_TOKENIZER_CONTINUE : GRN_TOKENIZER_LAST;
    if (tokenizer->id < tokenizer->tokens.size()) {
      const std::string &token = tokenizer->tokens[tokenizer->id++];
      grn_tokenizer_token_push(ctx, &tokenizer->token,
                               token.c_str(), token.length(), status);
    } else {
      grn_tokenizer_token_push(ctx, &tokenizer->token, "", 0, status);
    }
  }

  return NULL;
}

grn_obj *grn_kytea_fin(grn_ctx *ctx, int num_args, grn_obj **args,
                       grn_user_data *user_data) {
  grn_tokenizer_kytea * const tokenizer =
      static_cast<grn_tokenizer_kytea *>(user_data->ptr);
  if (tokenizer) {
    grn_tokenizer_kytea_fin(ctx, tokenizer);
    GRN_PLUGIN_FREE(ctx, tokenizer);
  }
  return NULL;
}

}  // namespace

extern "C" {

/*
  GRN_PLUGIN_INIT() is called to initialize this plugin. Note that an error
  code must be set in `ctx->rc' on failure.
 */
grn_rc GRN_PLUGIN_INIT(grn_ctx *ctx) {
  kytea_init(ctx);
  return ctx->rc;
}

/*
  GRN_PLUGIN_REGISTER() registers this plugin to the database associated with
  `ctx'. The registration requires the plugin name and the functions to be
  called for tokenization.
 */
grn_rc GRN_PLUGIN_REGISTER(grn_ctx *ctx) {
  return grn_tokenizer_register(ctx, "TokenKytea", 10, grn_kytea_init,
                                grn_kytea_next, grn_kytea_fin);
}

/*
  GRN_PLUGIN_FIN() is called to finalize the plugin that was initialized by
  GRN_PLUGIN_INIT().
 */
grn_rc GRN_PLUGIN_FIN(grn_ctx *ctx) {
  kytea_fin(ctx);
  return GRN_SUCCESS;
}

}  // extern "C"
