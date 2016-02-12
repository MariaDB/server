#ifndef NDBMEMCACHE_HASH_ITEM_UTIL_H
#define NDBMEMCACHE_HASH_ITEM_UTIL_H

#include <sys/types.h>
#include <stdint.h>
#include <arpa/inet.h>

#include <memcached/engine.h>

#define ITEM_WITH_CAS 1

struct default_engine;   // forward reference; needed in items.h

typedef struct _hash_item {
    struct _hash_item *next;
    struct _hash_item *prev;
    struct _hash_item *h_next; /* hash chain next */
    rel_time_t time;  /* least recent access */
    rel_time_t exptime; /**< When the item will expire (relative to process
                         * startup) */
    uint32_t nbytes; /**< The total size of the data (in bytes) */
    uint32_t flags; /**< Flags associated with the item (in network byte order)*/
    uint16_t nkey; /**< The total length of the key (in bytes) */
    uint16_t iflag; /**< Intermal flags. lower 8 bit is reserved for the core
                     * server, the upper 8 bits is reserved for engine
                     * implementation. */
    unsigned short refcount;
    uint8_t slabs_clsid;/* which slab class we're in */
} hash_item;

#ifdef    __cplusplus
extern "C" {
#endif

uint16_t hash_item_get_key_len(const hash_item *item);
uint32_t hash_item_get_data_len(const hash_item *item);
char * hash_item_get_key(const hash_item *item);
char * hash_item_get_data(const hash_item *item);
uint64_t hash_item_get_cas(const hash_item* item);
uint64_t hash_item_get_exp(const hash_item* item);
uint32_t hash_item_get_flag(const hash_item* item);
uint64_t * hash_item_get_cas_ptr(const hash_item* item);
void	hash_item_set_flag(hash_item* item, uint32_t value);
void	hash_item_set_cas(hash_item* item, uint64_t cas);

#ifdef    __cplusplus
}
#endif

#endif
