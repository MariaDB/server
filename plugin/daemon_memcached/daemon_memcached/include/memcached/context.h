/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#ifndef MEMCACHED_CONTEXT_H
#define MEMCACHED_CONTEXT_H

#include <stdlib.h>

/** Configuration info passed to memcached, including
the name of our Memcached InnoDB engine and memcached configure
string to be loaded by memcached. */
struct memcached_config
{
    char *option;
    char *engine_library;
    unsigned int r_batch_size;
    unsigned int w_batch_size;
    bool enable_binlog;
};

typedef struct memcached_config memcached_config_t;

struct memcached_container
{
    char *name;
};

typedef struct memcached_container memcached_container_t;

struct memcached_context
{
    memcached_config_t config;
    pthread_t thread;
    memcached_container_t *containers;
    unsigned int containers_number;
};

typedef struct memcached_context memcached_context_t;

#endif
