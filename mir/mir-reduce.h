/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#ifndef MIR_REDUCE_H
#define MIR_REDUCE_H

/* Data compression.  Major goals are simplicity, fast decompression
   speed, moderate compression speed.  The algorithm is tuned for
   binary MIR compression and close to LZ4.  Only we use a bit
   different format and offsets in symbol numbers instead of just
   offsets.

   A better compression (on par with LZ4) could be achieved by adding
   elements for all positions (now positions inside referenced symbols
   are excluded) or/and increasing the buffer or/and increasing the
   table.  But it would slow down the compression or/and increase the
   used memory.

   Functions reduce_encode, reduce_decode, reduce_encode_start,
   reduce_encode_put, reduce_encode_finish, reduce_decode_start,
   reduce_decode_get, reduce_decode_finish are the only interface
   functions.

   Format of compressed data: "MIR", elements*, zero byte, 8-byte check hash in little endian form
   Format of element:
    o 8 bits tag
      (N bits for symbol length; 0 means no sym, 2^N -1 means symbol length as uint present;
      (8-N) bits for reference length; 0 means no ref, 2^(8-N) - 1 means length as uint present)
    o [uint for symbol lenght]*, symbol string,
    o [uint for ref len]*, symbol number as uint */

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "mir-hash.h"
#include "mir-alloc.h"

#define FALSE 0
#define TRUE 1
#define _REDUCE_DATA_PREFIX "MIR" /* first chars of compressed data */
#define _REDUCE_SYMB_TAG_LEN 3    /* for some application could be 4 */
#define _REDUCE_SYMB_TAG_LONG ((1 << _REDUCE_SYMB_TAG_LEN) - 1) /* should be not changed */
#define _REDUCE_REF_TAG_LEN (8 - _REDUCE_SYMB_TAG_LEN)
#define _REDUCE_REF_TAG_LONG ((1 << _REDUCE_REF_TAG_LEN) - 1) /* should be not changed */
#define _REDUCE_START_LEN 4                                   /* Should be at least 4 */
#define _REDUCE_BUF_LEN (1 << 18)
/* The following should be power of two. There will be no space saving if it is less than 1/4 of buf
   length. */
#define _REDUCE_TABLE_SIZE (_REDUCE_BUF_LEN / 4)
#define _REDUCE_MAX_SYMB_LEN (2047)

typedef size_t (*reduce_reader_t) (void *start, size_t len, void *aux_data);
typedef size_t (*reduce_writer_t) (const void *start, size_t len, void *aux_data);

struct _reduce_el {
  uint32_t pos, num, next, head;
};

struct _reduce_encode_data {
  reduce_writer_t writer;
  uint32_t el_free;
  uint32_t curr_symb_len;
  uint8_t curr_symb[_REDUCE_MAX_SYMB_LEN];
  struct _reduce_el table[_REDUCE_TABLE_SIZE]; /* hash -> el */
};

struct _reduce_decode_data {
  uint8_t eof_p;
  uint32_t buf_get_pos;
  reduce_reader_t reader;
  uint32_t ind2pos[_REDUCE_BUF_LEN];
};

struct reduce_data {
  union {
    struct _reduce_encode_data encode;
    struct _reduce_decode_data decode;
  } u;
  void *aux_data;
  uint8_t ok_p;
  uint64_t check_hash;
  uint32_t curr_num, buf_bound;
  uint8_t buf[_REDUCE_BUF_LEN];
};

static inline uint32_t _reduce_min (uint32_t a, uint32_t b) { return a < b ? a : b; }

static inline uint32_t _reduce_get_new_el (struct reduce_data *data) {
  struct _reduce_encode_data *encode_data = &data->u.encode;
  uint32_t res = encode_data->el_free;

  if (res != UINT32_MAX) encode_data->el_free = encode_data->table[res].next;
  return res;
}

static inline void _reduce_put (struct reduce_data *data, int byte) {
  uint8_t u = byte;

  if (data->u.encode.writer (&u, 1, data->aux_data) != 1) data->ok_p = FALSE;
}

static inline int _reduce_get (reduce_reader_t reader, void *aux_data) {
  uint8_t u;

  if (reader (&u, 1, aux_data) != 1) return -1;
  return u;
}

static inline uint32_t _reduce_ref_offset_size (uint32_t offset) {
  return offset < (1 << 7) ? 1 : offset < (1 << 14) ? 2 : offset < (1 << 21) ? 3 : 4;
}

static inline uint32_t _reduce_ref_size (uint32_t len, uint32_t offset) {
  assert (len >= _REDUCE_START_LEN);
  len -= _REDUCE_START_LEN - 1;
  return ((len < _REDUCE_REF_TAG_LONG ? 0 : _reduce_ref_offset_size (len))
          + _reduce_ref_offset_size (offset));
}

static inline void _reduce_uint_write (struct reduce_data *data, uint32_t u) {
  int n;

  assert (u < (1 << 7 * 4));
  for (n = 1; n <= 4 && u >= (1u << 7 * n); n++)
    ;
  _reduce_put (data, (1 << (8 - n)) | ((u >> (n - 1) * 8) & 0xff)); /* tag */
  for (int i = 2; i <= n; i++) _reduce_put (data, (u >> (n - i) * 8) & 0xff);
}

static inline int64_t _reduce_uint_read (reduce_reader_t reader, void *aux_data) {
  int i, n, r = _reduce_get (reader, aux_data);
  uint32_t u, v;

  if (r < 0) return -1;
  for (u = (uint32_t) r, n = 1; n <= 4 && (u >> (8 - n)) != 1; n++)
    ;
  assert ((u >> (8 - n)) == 1);
  v = u & (0xff >> n);
  for (i = 1; i < n; i++) {
    if ((r = _reduce_get (reader, aux_data)) < 0) return -1;
    v = v * 256 + (uint32_t) r;
  }
  return v;
}

static inline void _reduce_hash_write (struct reduce_data *data, uint64_t h) {
  _reduce_put (data, 0); /* 0 tag */
  for (size_t i = 0; i < sizeof (uint64_t); i++) _reduce_put (data, (h >> i * 8) & 0xff);
}

static inline uint64_t _reduce_str2hash (const uint8_t *s) {
  uint64_t h = 0;

  for (size_t i = 0; i < sizeof (uint64_t); i++) h |= (uint64_t) s[i] << i * 8;
  return h;
}

static inline int _reduce_symb_flush (struct reduce_data *data, int ref_tag) {
  uint8_t u;
  struct _reduce_encode_data *encode_data = &data->u.encode;
  uint32_t len = encode_data->curr_symb_len;

  if (len == 0 && ref_tag == 0) return FALSE;
  u = ((len < _REDUCE_SYMB_TAG_LONG ? len : _REDUCE_SYMB_TAG_LONG) << _REDUCE_REF_TAG_LEN)
      | ref_tag;
  encode_data->writer (&u, 1, data->aux_data);
  if (len >= _REDUCE_SYMB_TAG_LONG) _reduce_uint_write (data, len);
  encode_data->writer (encode_data->curr_symb, len, data->aux_data);
  encode_data->curr_symb_len = 0;
  return TRUE;
}

static inline void _reduce_output_byte (struct reduce_data *data, uint32_t pos) {
  struct _reduce_encode_data *encode_data = &data->u.encode;

  if (encode_data->curr_symb_len + 1 > _REDUCE_MAX_SYMB_LEN) {
    _reduce_symb_flush (data, 0);
    encode_data->curr_symb_len = 0;
  }
  encode_data->curr_symb[encode_data->curr_symb_len++] = data->buf[pos];
}

static inline void _reduce_output_ref (struct reduce_data *data, uint32_t offset, uint32_t len) {
  uint32_t ref_tag;

  assert (len >= _REDUCE_START_LEN);
  len -= _REDUCE_START_LEN - 1;
  ref_tag = len < _REDUCE_REF_TAG_LONG ? len : _REDUCE_REF_TAG_LONG;
  _reduce_symb_flush (data, ref_tag);
  if (len >= _REDUCE_REF_TAG_LONG) _reduce_uint_write (data, len);
  _reduce_uint_write (data, offset);
}

#define _REDUCE_HASH_SEED 24

static inline uint32_t _reduce_dict_find_longest (struct reduce_data *data, uint32_t pos,
                                                  uint32_t *dict_pos) {
  uint32_t len, best_len, len_bound;
  uint64_t hash;
  uint32_t off, ref_size, best_ref_size = 0;
  uint32_t curr, next;
  const uint8_t *s1, *s2;
  struct _reduce_el *el, *best_el = NULL;
  struct _reduce_encode_data *encode_data = &data->u.encode;

  if (pos + _REDUCE_START_LEN > data->buf_bound) return 0;
  /* To have the same compressed output independently of the target
     and the used compiler, use strict hash even if it decreases
     compression speed by 10%.  */
  hash
    = mir_hash_strict (&data->buf[pos], _REDUCE_START_LEN, _REDUCE_HASH_SEED) % _REDUCE_TABLE_SIZE;
  best_len = 0; /* to remove a warning */
  for (curr = encode_data->table[hash].head; curr != UINT32_MAX; curr = next) {
    next = encode_data->table[curr].next;
    el = &encode_data->table[curr];
    len_bound = _reduce_min (data->buf_bound - pos, pos - el->pos);
    if (len_bound < _REDUCE_START_LEN) continue;
    s1 = &data->buf[el->pos];
    s2 = &data->buf[pos];
#if MIR_HASH_UNALIGNED_ACCESS
    assert (_REDUCE_START_LEN >= 4);
    if (*(uint32_t *) &s1[0] != *(uint32_t *) &s2[0]) continue;
    len = 4;
#else
    len = 0;
#endif
    for (; len < len_bound; len++)
      if (s1[len] != s2[len]) break;
#if !MIR_HASH_UNALIGNED_ACCESS
    if (len < _REDUCE_START_LEN) continue;
#endif
    off = data->curr_num - el->num;
    if (best_el == NULL) {
      best_len = len;
      best_el = el;
      best_ref_size = _reduce_ref_size (len, off);
      continue;
    }
    ref_size = _reduce_ref_size (len, off);
    if (best_len + ref_size < len + best_ref_size) {
      best_len = len;
      best_el = el;
      best_ref_size = ref_size;
    }
  }
  if (best_el == NULL) return 0;
  *dict_pos = best_el->num;
  return best_len;
}

static inline void _reduce_dict_add (struct reduce_data *data, uint32_t pos) {
  uint64_t hash;
  uint32_t prev, curr, num = data->curr_num++;
  struct _reduce_encode_data *encode_data = &data->u.encode;

  if (pos + _REDUCE_START_LEN > data->buf_bound) return;
  hash
    = mir_hash_strict (&data->buf[pos], _REDUCE_START_LEN, _REDUCE_HASH_SEED) % _REDUCE_TABLE_SIZE;
  if ((curr = _reduce_get_new_el (data)) == UINT32_MAX) { /* rare case: use last if any */
    for (prev = UINT32_MAX, curr = encode_data->table[hash].head;
         curr != UINT32_MAX && encode_data->table[curr].next != UINT32_MAX;
         prev = curr, curr = encode_data->table[curr].next)
      ;
    if (curr == UINT32_MAX) return; /* no more free els */
    if (prev != UINT32_MAX)
      encode_data->table[prev].next = encode_data->table[curr].next;
    else
      encode_data->table[hash].head = encode_data->table[curr].next;
  }
  encode_data->table[curr].pos = pos;
  encode_data->table[curr].num = num;
  encode_data->table[curr].next = encode_data->table[hash].head;
  encode_data->table[hash].head = curr;
}

static void _reduce_reset_next (struct reduce_data *data) {
  struct _reduce_encode_data *encode_data = &data->u.encode;

  for (uint32_t i = 0; i < _REDUCE_TABLE_SIZE; i++) {
    encode_data->table[i].next = i + 1;
    encode_data->table[i].head = UINT32_MAX;
  }
  encode_data->table[_REDUCE_TABLE_SIZE - 1].next = UINT32_MAX;
  encode_data->el_free = 0;
}

#define _REDUCE_CHECK_HASH_SEED 42

static inline struct reduce_data *reduce_encode_start (MIR_alloc_t alloc, reduce_writer_t writer,
                                                       void *aux_data) {
  struct reduce_data *data = MIR_malloc (alloc, sizeof (struct reduce_data));
  char prefix[] = _REDUCE_DATA_PREFIX;
  size_t prefix_size = strlen (prefix);

  if (data == NULL) return data;
  data->u.encode.writer = writer;
  data->aux_data = aux_data;
  data->check_hash = _REDUCE_CHECK_HASH_SEED;
  data->buf_bound = 0;
  data->ok_p = writer (prefix, prefix_size, aux_data) == prefix_size;
  return data;
}

static inline void _reduce_encode_buf (struct reduce_data *data) {
  uint32_t dict_len, dict_pos, base;

  if (data->buf_bound == 0) return;
  data->check_hash = mir_hash_strict (data->buf, data->buf_bound, data->check_hash);
  data->curr_num = data->u.encode.curr_symb_len = 0;
  _reduce_reset_next (data);
  for (uint32_t pos = 0; pos < data->buf_bound;) {
    dict_len = _reduce_dict_find_longest (data, pos, &dict_pos);
    base = data->curr_num;
    if (dict_len == 0) {
      _reduce_output_byte (data, pos);
      _reduce_dict_add (data, pos);
      pos++;
      continue;
    }
    _reduce_output_ref (data, base - dict_pos, dict_len);
    _reduce_dict_add (data, pos); /* replace */
    pos += dict_len;
  }
  _reduce_symb_flush (data, 0);
}

static inline void reduce_encode_put (struct reduce_data *data, int c) {
  if (data->buf_bound < _REDUCE_BUF_LEN) {
    data->buf[data->buf_bound++] = c;
    return;
  }
  _reduce_encode_buf (data);
  data->buf_bound = 0;
  data->buf[data->buf_bound++] = c;
}

static inline int reduce_encode_finish (MIR_alloc_t alloc, struct reduce_data *data) {
  int ok_p;

  _reduce_encode_buf (data);
  _reduce_hash_write (data, data->check_hash);
  ok_p = data->ok_p;
  MIR_free (alloc, data);
  return ok_p;
}

static inline struct reduce_data *reduce_decode_start (MIR_alloc_t alloc, reduce_reader_t reader,
                                                       void *aux_data) {
  struct reduce_data *data = MIR_malloc (alloc, sizeof (struct reduce_data));
  struct _reduce_decode_data *decode_data = &data->u.decode;
  char prefix[] = _REDUCE_DATA_PREFIX, str[sizeof (prefix)];
  size_t prefix_size = strlen (prefix);

  if (data == NULL) return data;
  decode_data->reader = reader;
  data->aux_data = aux_data;
  data->check_hash = _REDUCE_CHECK_HASH_SEED;
  decode_data->buf_get_pos = data->buf_bound = 0;
  data->ok_p
    = reader (str, prefix_size, aux_data) == prefix_size && memcmp (prefix, str, prefix_size) == 0;
  decode_data->eof_p = FALSE;
  return data;
}

static inline int reduce_decode_get (struct reduce_data *data) {
  uint8_t tag, hash_str[sizeof (uint64_t)];
  uint32_t sym_len, ref_len, ref_ind, sym_pos, pos = 0, curr_ind = 0;
  int64_t r;
  struct _reduce_decode_data *decode_data = &data->u.decode;
  reduce_reader_t reader = decode_data->reader;

  if (decode_data->buf_get_pos < data->buf_bound) return data->buf[decode_data->buf_get_pos++];
  if (decode_data->eof_p) return -1;
  for (;;) {
    if (reader (&tag, 1, data->aux_data) == 0) break;
    if (tag == 0) { /* check hash */
      if (reader (hash_str, sizeof (hash_str), data->aux_data) != sizeof (hash_str)
          || reader (&tag, 1, data->aux_data) != 0)
        break;
      if (pos != 0) data->check_hash = mir_hash_strict (data->buf, pos, data->check_hash);
      if (_reduce_str2hash (hash_str) != data->check_hash) break;
      decode_data->eof_p = TRUE;
      decode_data->buf_get_pos = 0;
      data->buf_bound = pos;
      return pos == 0 ? -1 : data->buf[decode_data->buf_get_pos++];
    }
    sym_len = tag >> _REDUCE_REF_TAG_LEN;
    if (sym_len != 0) {
      if (sym_len == _REDUCE_SYMB_TAG_LONG) {
        if ((r = _reduce_uint_read (reader, data->aux_data)) < 0) break;
        sym_len = (uint32_t) r;
      }
      if (sym_len > _REDUCE_MAX_SYMB_LEN || pos + sym_len > _REDUCE_BUF_LEN) break;
      if (reader (&data->buf[pos], sym_len, data->aux_data) != sym_len) break;
      for (uint32_t i = 0; i < sym_len; i++, pos++, curr_ind++)
        decode_data->ind2pos[curr_ind] = pos;
    }
    ref_len = tag & _REDUCE_REF_TAG_LONG;
    if (ref_len != 0) {
      if (ref_len == _REDUCE_REF_TAG_LONG) {
        if ((r = _reduce_uint_read (reader, data->aux_data)) < 0) break;
        ref_len = (uint32_t) r;
      }
      ref_len += _REDUCE_START_LEN - 1;
      if ((r = _reduce_uint_read (reader, data->aux_data)) < 0) break;
      ref_ind = (uint32_t) r;
      if (curr_ind < ref_ind) break;
      sym_pos = decode_data->ind2pos[curr_ind - ref_ind];
      if (sym_pos + ref_len > _REDUCE_BUF_LEN) break;
      memcpy (&data->buf[pos], &data->buf[sym_pos], ref_len);
      decode_data->ind2pos[curr_ind++] = pos;
      pos += ref_len;
    }
    if (pos >= _REDUCE_BUF_LEN) {
      assert (pos == _REDUCE_BUF_LEN);
      data->check_hash = mir_hash_strict (data->buf, pos, data->check_hash);
      data->buf_bound = _REDUCE_BUF_LEN;
      decode_data->buf_get_pos = 0;
      return data->buf[decode_data->buf_get_pos++];
    }
  }
  data->ok_p = FALSE;
  return -1;
}

static inline int reduce_decode_finish (MIR_alloc_t alloc, struct reduce_data *data) {
  uint8_t tag;
  int ok_p
    = data->ok_p && data->u.decode.eof_p && data->u.decode.reader (&tag, 1, data->aux_data) == 0;

  MIR_free (alloc, data);
  return ok_p;
}

#define _REDUCE_WRITE_IO_LEN 256
static inline int reduce_encode (MIR_alloc_t alloc, reduce_reader_t reader, reduce_writer_t writer,
                                 void *aux_data) {
  size_t i, size;
  uint8_t buf[_REDUCE_WRITE_IO_LEN];
  struct reduce_data *data = reduce_encode_start (alloc, writer, aux_data);

  if (data == NULL) return FALSE;
  for (;;) {
    if ((size = reader (buf, _REDUCE_WRITE_IO_LEN, data->aux_data)) == 0) break;
    for (i = 0; i < size; i++) reduce_encode_put (data, buf[i]);
  }
  return reduce_encode_finish (alloc, data);
}

static inline int reduce_decode (MIR_alloc_t alloc, reduce_reader_t reader, reduce_writer_t writer,
                                 void *aux_data) {
  int c, i;
  uint8_t buf[_REDUCE_WRITE_IO_LEN];
  struct reduce_data *data = reduce_decode_start (alloc, reader, aux_data);

  if (data == NULL) return FALSE;
  for (;;) {
    for (i = 0; i < _REDUCE_WRITE_IO_LEN && (c = reduce_decode_get (data)) >= 0; i++) buf[i] = c;
    if (i != 0) writer (buf, i, aux_data);
    if (c < 0) break;
  }
  return reduce_decode_finish (alloc, data);
}

#endif /* #ifndef MIR_REDUCE_H */
