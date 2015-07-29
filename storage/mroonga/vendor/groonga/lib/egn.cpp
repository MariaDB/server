/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifdef GRN_WITH_EGN

#include "grn_egn.hpp"

#include <cctype>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>

#include <iostream>  // for debug!

#include "grn_ctx_impl.h"
#include "grn_db.h"
#include "grn_output.h"
#include "grn_str.h"

// TODO: Error handling.

namespace {

enum { GRN_EGN_MAX_BATCH_SIZE = 1024 };

bool grn_egn_is_table_cursor(grn_obj *obj) {
  if (!obj) {
    return false;
  }
  switch (obj->header.type) {
    case GRN_CURSOR_TABLE_PAT_KEY:
    case GRN_CURSOR_TABLE_DAT_KEY:
    case GRN_CURSOR_TABLE_HASH_KEY:
    case GRN_CURSOR_TABLE_NO_KEY: {
      return true;
    }
    default: {
      return false;
    }
  }
}

bool grn_egn_is_table(grn_obj *obj) {
  if (!obj) {
    return false;
  }
  switch (obj->header.type) {
    case GRN_TABLE_HASH_KEY:
    case GRN_TABLE_PAT_KEY:
    case GRN_TABLE_DAT_KEY:
    case GRN_TABLE_NO_KEY: {
      return true;
    }
    default: {
      return false;
    }
  }
}

}  // namespace

namespace grn {
namespace egn {

// -- TableCursor --

// TableCursor is a wrapper for grn_table_cursor:
// - GRN_CURSOR_PAT_KEY
// - GRN_CURSOR_DAT_KEY
// - GRN_CURSOR_HASH_KEY
// - GRN_CURSOR_NO_KEY
class TableCursor : public Cursor {
 public:
  ~TableCursor() {
    grn_table_cursor_close(ctx_, cursor_);
  }

  static grn_rc open(grn_ctx *ctx, grn_obj *cursor, Score default_score,
                     Cursor **wrapper) {
    if (!ctx || !grn_egn_is_table_cursor(cursor) || !wrapper) {
      return GRN_INVALID_ARGUMENT;
    }
    TableCursor *new_wrapper =
      new (std::nothrow) TableCursor(ctx, cursor, default_score);
    if (!new_wrapper) {
      return GRN_NO_MEMORY_AVAILABLE;
    }
    *wrapper = new_wrapper;
    return GRN_SUCCESS;
  }

  grn_rc read(Record *records, size_t size, size_t *count);

 private:
  grn_ctx *ctx_;
  grn_obj *cursor_;
  Score default_score_;

  TableCursor(grn_ctx *ctx, grn_obj *cursor, Score default_score)
    : Cursor(), ctx_(ctx), cursor_(cursor), default_score_(default_score) {}
};

grn_rc TableCursor::read(Record *records, size_t size, size_t *count) {
  if ((!records && (size != 0)) || !count) {
    return GRN_INVALID_ARGUMENT;
  }
  switch (cursor_->header.type) {
    case GRN_CURSOR_TABLE_PAT_KEY: {
      for (size_t i = 0; i < size; ++i) {
        grn_id id = grn_pat_cursor_next(
          ctx_, reinterpret_cast<grn_pat_cursor *>(cursor_));
        if (id == GRN_ID_NIL) {
          *count = i;
          return GRN_SUCCESS;
        }
        records[i].id = id;
        records[i].score = default_score_;
      }
      break;
    }
    case GRN_CURSOR_TABLE_DAT_KEY: {
      for (size_t i = 0; i < size; ++i) {
        grn_id id = grn_dat_cursor_next(
          ctx_, reinterpret_cast<grn_dat_cursor *>(cursor_));
        if (id == GRN_ID_NIL) {
          *count = i;
          return GRN_SUCCESS;
        }
        records[i].id = id;
        records[i].score = default_score_;
      }
      break;
    }
    case GRN_CURSOR_TABLE_HASH_KEY: {
      for (size_t i = 0; i < size; ++i) {
        grn_id id = grn_hash_cursor_next(
          ctx_, reinterpret_cast<grn_hash_cursor *>(cursor_));
        if (id == GRN_ID_NIL) {
          *count = i;
          return GRN_SUCCESS;
        }
        records[i].id = id;
        records[i].score = default_score_;
      }
      break;
    }
    case GRN_CURSOR_TABLE_NO_KEY: {
      for (size_t i = 0; i < size; ++i) {
        grn_id id = grn_array_cursor_next(
          ctx_, reinterpret_cast<grn_array_cursor *>(cursor_));
        if (id == GRN_ID_NIL) {
          *count = i;
          return GRN_SUCCESS;
        }
        records[i].id = id;
        records[i].score = default_score_;
      }
      break;
    }
    default: {
      return GRN_UNKNOWN_ERROR;
    }
  }
  *count = size;
  return GRN_SUCCESS;
}

// -- Cursor --

grn_rc Cursor::open_table_cursor(
  grn_ctx *ctx, grn_obj *table, Cursor **cursor) {
  if (!ctx || !grn_egn_is_table(table) || !cursor) {
    return GRN_INVALID_ARGUMENT;
  }
  grn_table_cursor *table_cursor = grn_table_cursor_open(
    ctx, table, NULL, 0, NULL, 0, 0, -1,
    GRN_CURSOR_ASCENDING | GRN_CURSOR_BY_ID);
  if (!table_cursor) {
    return ctx->rc;
  }
  grn_rc rc = TableCursor::open(ctx, table_cursor, 0.0, cursor);
  if (rc != GRN_SUCCESS) {
    grn_table_cursor_close(ctx, table_cursor);
  }
  return rc;
}

grn_rc Cursor::read(Record *records, size_t size, size_t *count) {
  if ((!records && (size != 0)) || !count) {
    return GRN_INVALID_ARGUMENT;
  }
  *count = 0;
  return GRN_SUCCESS;
}

// -- ExpressionNode --

class ExpressionNode {
 public:
  ExpressionNode() {}
  virtual ~ExpressionNode() {}

  virtual ExpressionNodeType type() const = 0;
  virtual DataType data_type() const = 0;

  virtual grn_rc filter(Record *input, size_t input_size,
                        Record *output, size_t *output_size) {
    return GRN_OPERATION_NOT_SUPPORTED;
  }
  virtual grn_rc adjust(Record *records, size_t num_records) {
    return GRN_OPERATION_NOT_SUPPORTED;
  }
};

// -- TypedNode<T> --

template <typename T>
class TypedNode : public ExpressionNode {
 public:
  TypedNode() : ExpressionNode() {}
  virtual ~TypedNode() {}

  DataType data_type() const {
    return T::data_type();
  }

  virtual grn_rc evaluate(
    const Record *records, size_t num_records, T *results) = 0;
};

// -- TypedNode<Bool> --

template <>
class TypedNode<Bool> : public ExpressionNode {
 public:
  TypedNode() : ExpressionNode(), values_for_filter_() {}
  virtual ~TypedNode() {}

  DataType data_type() const {
    return Bool::data_type();
  }

  virtual grn_rc filter(Record *input, size_t input_size,
                        Record *output, size_t *output_size);

  virtual grn_rc evaluate(
    const Record *records, size_t num_records, Bool *results) = 0;

 private:
  std::vector<Bool> values_for_filter_;
};

grn_rc TypedNode<Bool>::filter(Record *input, size_t input_size,
                               Record *output, size_t *output_size) {
  if (values_for_filter_.size() < input_size) {
    values_for_filter_.resize(input_size);
  }
  grn_rc rc = evaluate(input, input_size, &*values_for_filter_.begin());
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  size_t count = 0;
  for (size_t i = 0; i < input_size; ++i) {
    if (values_for_filter_[i].raw) {
      output[count] = input[i];
      ++count;
    }
  }
  *output_size = count;
  return GRN_SUCCESS;
}

// -- TypedNode<Float> --

template <>
class TypedNode<Float> : public ExpressionNode {
 public:
  TypedNode() : ExpressionNode(), values_for_adjust_() {}
  virtual ~TypedNode() {}

  DataType data_type() const {
    return Float::data_type();
  }

  virtual grn_rc adjust(Record *records, size_t num_records);

  virtual grn_rc evaluate(
    const Record *records, size_t num_records, Float *results) = 0;

 private:
  std::vector<Float> values_for_adjust_;
};

grn_rc TypedNode<Float>::adjust(Record *records, size_t num_records) {
  if (values_for_adjust_.size() < num_records) {
    values_for_adjust_.resize(num_records);
  }
  grn_rc rc = evaluate(records, num_records, &*values_for_adjust_.begin());
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  for (size_t i = 0; i < num_records; ++i) {
    records[i].score = values_for_adjust_[i].raw;
  }
  return GRN_SUCCESS;
}

// -- IDNode --

class IDNode : public TypedNode<Int> {
 public:
  ~IDNode() {}

  static grn_rc open(ExpressionNode **node) {
    ExpressionNode *new_node = new (std::nothrow) IDNode();
    if (!new_node) {
      return GRN_NO_MEMORY_AVAILABLE;
    }
    *node = new_node;
    return GRN_SUCCESS;
  }

  ExpressionNodeType type() const {
    return GRN_EGN_ID_NODE;
  }

  grn_rc evaluate(
    const Record *records, size_t num_records, Int *results) {
    for (size_t i = 0; i < num_records; ++i) {
      results[i] = Int(records[i].id);
    }
    return GRN_SUCCESS;
  }

 private:
  IDNode() : TypedNode<Int>() {}
};

// -- ScoreNode --

class ScoreNode : public TypedNode<Float> {
 public:
  ~ScoreNode() {}

  static grn_rc open(ExpressionNode **node) {
    ExpressionNode *new_node = new (std::nothrow) ScoreNode();
    if (!new_node) {
      return GRN_NO_MEMORY_AVAILABLE;
    }
    *node = new_node;
    return GRN_SUCCESS;
  }

  ExpressionNodeType type() const {
    return GRN_EGN_SCORE_NODE;
  }

  grn_rc evaluate(
    const Record *records, size_t num_records, Float *results) {
    for (size_t i = 0; i < num_records; ++i) {
      results[i] = Float(records[i].score);
    }
    return GRN_SUCCESS;
  }

 private:
  ScoreNode() : TypedNode<Float>() {}
};

// -- ConstantNode<T> --

template <typename T>
class ConstantNode : public TypedNode<T> {
 public:
  ~ConstantNode() {}

  static grn_rc open(const T &value, ExpressionNode **node) {
    ConstantNode *new_node = new (std::nothrow) ConstantNode(value);
    if (!new_node) {
      return GRN_NO_MEMORY_AVAILABLE;
    }
    *node = new_node;
    return GRN_SUCCESS;
  }

  ExpressionNodeType type() const {
    return GRN_EGN_CONSTANT_NODE;
  }

  grn_rc evaluate(
    const Record *records, size_t num_records, T *results) {
    for (size_t i = 0; i < num_records; ++i) {
      results[i] = value_;
    }
    return GRN_SUCCESS;
  }

 private:
  T value_;

  explicit ConstantNode(const T &value) : TypedNode<T>(), value_(value) {}
};

// -- ConstantNode<Bool> --

template <>
class ConstantNode<Bool> : public TypedNode<Bool> {
 public:
  ~ConstantNode() {}

  static grn_rc open(Bool value, ExpressionNode **node) {
    ConstantNode *new_node = new (std::nothrow) ConstantNode(value);
    if (!new_node) {
      return GRN_NO_MEMORY_AVAILABLE;
    }
    *node = new_node;
    return GRN_SUCCESS;
  }

  ExpressionNodeType type() const {
    return GRN_EGN_CONSTANT_NODE;
  }

  grn_rc filter(Record *input, size_t input_size,
                Record *output, size_t *output_size);

  grn_rc evaluate(
    const Record *records, size_t num_records, Bool *results) {
    for (size_t i = 0; i < num_records; ++i) {
      results[i] = value_;
    }
    return GRN_SUCCESS;
  }

 private:
  Bool value_;

  explicit ConstantNode(Bool value) : TypedNode<Bool>(), value_(value) {}
};

grn_rc ConstantNode<Bool>::filter(
  Record *input, size_t input_size,
  Record *output, size_t *output_size) {
  if (value_.raw == GRN_TRUE) {
    // The I/O areas are the same and there is no need to copy records.
    if (input != output) {
      for (size_t i = 0; i < input_size; ++i) {
        output[i] = input[i];
      }
    }
    *output_size = input_size;
  } else {
    *output_size = 0;
  }
  return GRN_SUCCESS;
}

// -- ConstantNode<Float> --

template <>
class ConstantNode<Float> : public TypedNode<Float> {
 public:
  ~ConstantNode() {}

  static grn_rc open(Float value, ExpressionNode **node) {
    ConstantNode *new_node = new (std::nothrow) ConstantNode(value);
    if (!new_node) {
      return GRN_NO_MEMORY_AVAILABLE;
    }
    *node = new_node;
    return GRN_SUCCESS;
  }

  ExpressionNodeType type() const {
    return GRN_EGN_CONSTANT_NODE;
  }

  grn_rc adjust(Record *records, size_t num_records) {
    for (size_t i = 0; i < num_records; ++i) {
      records[i].score = value_.raw;
    }
    return GRN_SUCCESS;
  }

  grn_rc evaluate(
    const Record *records, size_t num_records, Float *results) {
    for (size_t i = 0; i < num_records; ++i) {
      results[i] = value_;
    }
    return GRN_SUCCESS;
  }

 private:
  Float value_;

  explicit ConstantNode(Float value) : TypedNode<Float>(), value_(value) {}
};

// -- ConstantNode<Text> --

template <>
class ConstantNode<Text> : public TypedNode<Text> {
 public:
  ~ConstantNode() {}

  static grn_rc open(const Text &value, ExpressionNode **node) {
    ConstantNode *new_node = new (std::nothrow) ConstantNode(value);
    if (!new_node) {
      return GRN_NO_MEMORY_AVAILABLE;
    }
    try {
      new_node->value_buf_.resize(value.raw.size);
    } catch (const std::bad_alloc &) {
      delete new_node;
      return GRN_NO_MEMORY_AVAILABLE;
    }
    std::memcpy(&*new_node->value_buf_.begin(), value.raw.ptr, value.raw.size);
    new_node->value_.raw.ptr = &*new_node->value_buf_.begin();
    *node = new_node;
    return GRN_SUCCESS;
  }

  ExpressionNodeType type() const {
    return GRN_EGN_CONSTANT_NODE;
  }

  grn_rc evaluate(
    const Record *records, size_t num_records, Text *results) {
    for (size_t i = 0; i < num_records; ++i) {
      results[i] = value_;
    }
    return GRN_SUCCESS;
  }

 private:
  Text value_;
  std::vector<char> value_buf_;

  explicit ConstantNode(const Text &value)
    : TypedNode<Text>(), value_(value), value_buf_() {}
};

// -- ColumnNode --

template <typename T>
class ColumnNode : public TypedNode<T> {
 public:
  ~ColumnNode() {}

  static grn_rc open(grn_ctx *ctx, grn_obj *column, ExpressionNode **node) {
    ColumnNode *new_node = new (std::nothrow) ColumnNode(ctx, column);
    if (!new_node) {
      return GRN_NO_MEMORY_AVAILABLE;
    }
    *node = new_node;
    return GRN_SUCCESS;
  }

  ExpressionNodeType type() const {
    return GRN_EGN_COLUMN_NODE;
  }

  grn_rc evaluate(
    const Record *records, size_t num_records, T *results) {
    // TODO
    return GRN_OPERATION_NOT_SUPPORTED;
  }

 private:
  grn_ctx *ctx_;
  grn_obj *column_;

  ColumnNode(grn_ctx *ctx, grn_obj *column)
    : TypedNode<T>(), ctx_(ctx), column_(column) {}
};

// -- ColumnNode<Bool> --

template <>
class ColumnNode<Bool> : public TypedNode<Bool> {
 public:
  ~ColumnNode() {}

  static grn_rc open(grn_ctx *ctx, grn_obj *column, ExpressionNode **node) {
    ColumnNode *new_node = new (std::nothrow) ColumnNode(ctx, column);
    if (!new_node) {
      return GRN_NO_MEMORY_AVAILABLE;
    }
    *node = new_node;
    return GRN_SUCCESS;
  }

  ExpressionNodeType type() const {
    return GRN_EGN_COLUMN_NODE;
  }

  grn_rc filter(
    Record *input, size_t input_size,
    Record *output, size_t *output_size);

  grn_rc evaluate(
    const Record *records, size_t num_records, Bool *results);

 private:
  grn_ctx *ctx_;
  grn_obj *column_;

  ColumnNode(grn_ctx *ctx, grn_obj *column)
    : TypedNode<Bool>(), ctx_(ctx), column_(column) {}
};

grn_rc ColumnNode<Bool>::filter(
  Record *input, size_t input_size,
  Record *output, size_t *output_size) {
  grn_obj value;
  GRN_BOOL_INIT(&value, 0);
  size_t count = 0;
  for (size_t i = 0; i < input_size; ++i) {
    GRN_BULK_REWIND(&value);
    grn_obj_get_value(ctx_, column_, input[i].id, &value);
    if (ctx_->rc != GRN_SUCCESS) {
      return ctx_->rc;
    }
    if (GRN_BOOL_VALUE(&value) == GRN_TRUE) {
      output[count] = input[i];
      ++count;
    }
  }
  GRN_OBJ_FIN(ctx_, &value);
  *output_size = count;
  return GRN_SUCCESS;
}

grn_rc ColumnNode<Bool>::evaluate(
  const Record *records, size_t num_records, Bool *results) {
  grn_obj value;
  GRN_BOOL_INIT(&value, 0);
  for (size_t i = 0; i < num_records; i++) {
    GRN_BULK_REWIND(&value);
    grn_obj_get_value(ctx_, column_, records[i].id, &value);
    if (ctx_->rc != GRN_SUCCESS) {
      return ctx_->rc;
    }
    results[i] = Bool(GRN_BOOL_VALUE(&value) == GRN_TRUE);
  }
  GRN_OBJ_FIN(ctx_, &value);
  return GRN_SUCCESS;
}

// -- ColumnNode<Int> --

template <>
class ColumnNode<Int> : public TypedNode<Int> {
 public:
  ~ColumnNode() {}

  static grn_rc open(grn_ctx *ctx, grn_obj *column, ExpressionNode **node) {
    ColumnNode *new_node = new (std::nothrow) ColumnNode(ctx, column);
    if (!new_node) {
      return GRN_NO_MEMORY_AVAILABLE;
    }
    *node = new_node;
    return GRN_SUCCESS;
  }

  ExpressionNodeType type() const {
    return GRN_EGN_COLUMN_NODE;
  }

  grn_rc evaluate(
    const Record *records, size_t num_records, Int *results);

 private:
  grn_ctx *ctx_;
  grn_obj *column_;

  ColumnNode(grn_ctx *ctx, grn_obj *column)
    : TypedNode<Int>(), ctx_(ctx), column_(column) {}
};

grn_rc ColumnNode<Int>::evaluate(
  const Record *records, size_t num_records, Int *results) {
  grn_id range = grn_obj_get_range(ctx_, column_);
  grn_obj value;
  switch (range) {
    case GRN_DB_INT8: {
      GRN_INT8_INIT(&value, 0);
      for (size_t i = 0; i < num_records; i++) {
        GRN_BULK_REWIND(&value);
        grn_obj_get_value(ctx_, column_, records[i].id, &value);
        results[i] = Int(GRN_INT8_VALUE(&value));
      }
      break;
    }
    case GRN_DB_INT16: {
      GRN_INT16_INIT(&value, 0);
      for (size_t i = 0; i < num_records; i++) {
        GRN_BULK_REWIND(&value);
        grn_obj_get_value(ctx_, column_, records[i].id, &value);
        results[i] = Int(GRN_INT16_VALUE(&value));
      }
      break;
    }
    case GRN_DB_INT32: {
      GRN_INT32_INIT(&value, 0);
      for (size_t i = 0; i < num_records; i++) {
        GRN_BULK_REWIND(&value);
        grn_obj_get_value(ctx_, column_, records[i].id, &value);
        results[i] = Int(GRN_INT32_VALUE(&value));
      }
      break;
    }
    case GRN_DB_INT64: {
      GRN_INT64_INIT(&value, 0);
      for (size_t i = 0; i < num_records; i++) {
        GRN_BULK_REWIND(&value);
        grn_obj_get_value(ctx_, column_, records[i].id, &value);
        results[i] = Int(GRN_INT64_VALUE(&value));
      }
      break;
    }
    case GRN_DB_UINT8: {
      GRN_UINT8_INIT(&value, 0);
      for (size_t i = 0; i < num_records; i++) {
        GRN_BULK_REWIND(&value);
        grn_obj_get_value(ctx_, column_, records[i].id, &value);
        results[i] = Int(GRN_UINT8_VALUE(&value));
      }
      break;
    }
    case GRN_DB_UINT16: {
      GRN_UINT16_INIT(&value, 0);
      for (size_t i = 0; i < num_records; i++) {
        GRN_BULK_REWIND(&value);
        grn_obj_get_value(ctx_, column_, records[i].id, &value);
        results[i] = Int(GRN_UINT16_VALUE(&value));
      }
      break;
    }
    case GRN_DB_UINT32: {
      GRN_UINT32_INIT(&value, 0);
      for (size_t i = 0; i < num_records; i++) {
        GRN_BULK_REWIND(&value);
        grn_obj_get_value(ctx_, column_, records[i].id, &value);
        results[i] = Int(GRN_UINT32_VALUE(&value));
      }
      break;
    }
    case GRN_DB_UINT64: {
      GRN_UINT64_INIT(&value, 0);
      for (size_t i = 0; i < num_records; i++) {
        GRN_BULK_REWIND(&value);
        grn_obj_get_value(ctx_, column_, records[i].id, &value);
        // FIXME: Type conversion from UInt64 to Int may lose the content.
        results[i] = Int(GRN_UINT64_VALUE(&value));
      }
      break;
    }
  }
  GRN_OBJ_FIN(ctx_, &value);
  return GRN_SUCCESS;
}

// -- ColumnNode<Float> --

template <>
class ColumnNode<Float> : public TypedNode<Float> {
 public:
  ~ColumnNode() {}

  static grn_rc open(grn_ctx *ctx, grn_obj *column, ExpressionNode **node) {
    ColumnNode *new_node = new (std::nothrow) ColumnNode(ctx, column);
    if (!new_node) {
      return GRN_NO_MEMORY_AVAILABLE;
    }
    *node = new_node;
    return GRN_SUCCESS;
  }

  ExpressionNodeType type() const {
    return GRN_EGN_COLUMN_NODE;
  }

  grn_rc adjust(Record *records, size_t num_records);

  grn_rc evaluate(
    const Record *records, size_t num_records, Float *results);

 private:
  grn_ctx *ctx_;
  grn_obj *column_;

  ColumnNode(grn_ctx *ctx, grn_obj *column)
    : TypedNode<Float>(), ctx_(ctx), column_(column) {}
};

grn_rc ColumnNode<Float>::adjust(Record *records, size_t num_records) {
  grn_obj value;
  GRN_FLOAT_INIT(&value, 0);
  for (size_t i = 0; i < num_records; ++i) {
    GRN_BULK_REWIND(&value);
    grn_obj_get_value(ctx_, column_, records[i].id, &value);
    records[i].score = GRN_FLOAT_VALUE(&value);
  }
  GRN_OBJ_FIN(ctx_, &value);
  return GRN_SUCCESS;
}

grn_rc ColumnNode<Float>::evaluate(
  const Record *records, size_t num_records, Float *results) {
  grn_obj value;
  GRN_FLOAT_INIT(&value, 0);
  for (size_t i = 0; i < num_records; i++) {
    GRN_BULK_REWIND(&value);
    grn_obj_get_value(ctx_, column_, records[i].id, &value);
    results[i] = Float(GRN_FLOAT_VALUE(&value));
  }
  GRN_OBJ_FIN(ctx_, &value);
  return GRN_SUCCESS;
}

// -- ColumnNode<Time> --

template <>
class ColumnNode<Time> : public TypedNode<Time> {
 public:
  ~ColumnNode() {}

  static grn_rc open(grn_ctx *ctx, grn_obj *column, ExpressionNode **node) {
    ColumnNode *new_node = new (std::nothrow) ColumnNode(ctx, column);
    if (!new_node) {
      return GRN_NO_MEMORY_AVAILABLE;
    }
    *node = new_node;
    return GRN_SUCCESS;
  }

  ExpressionNodeType type() const {
    return GRN_EGN_COLUMN_NODE;
  }

  grn_rc evaluate(
    const Record *records, size_t num_records, Time *results);

 private:
  grn_ctx *ctx_;
  grn_obj *column_;

  ColumnNode(grn_ctx *ctx, grn_obj *column)
    : TypedNode<Time>(), ctx_(ctx), column_(column) {}
};

grn_rc ColumnNode<Time>::evaluate(
  const Record *records, size_t num_records, Time *results) {
  grn_obj value;
  GRN_TIME_INIT(&value, 0);
  for (size_t i = 0; i < num_records; i++) {
    GRN_BULK_REWIND(&value);
    grn_obj_get_value(ctx_, column_, records[i].id, &value);
    results[i] = Time(GRN_TIME_VALUE(&value));
  }
  GRN_OBJ_FIN(ctx_, &value);
  return GRN_SUCCESS;
}

// -- ColumnNode<Text> --

template <>
class ColumnNode<Text> : public TypedNode<Text> {
 public:
  ~ColumnNode() {
    GRN_OBJ_FIN(ctx_, &buf_);
  }

  static grn_rc open(grn_ctx *ctx, grn_obj *column, ExpressionNode **node) {
    ColumnNode *new_node = new (std::nothrow) ColumnNode(ctx, column);
    if (!new_node) {
      return GRN_NO_MEMORY_AVAILABLE;
    }
    *node = new_node;
    return GRN_SUCCESS;
  }

  ExpressionNodeType type() const {
    return GRN_EGN_COLUMN_NODE;
  }

  grn_rc evaluate(
    const Record *records, size_t num_records, Text *results);

 private:
  grn_ctx *ctx_;
  grn_obj *column_;
  grn_obj buf_;

  ColumnNode(grn_ctx *ctx, grn_obj *column)
    : TypedNode<Text>(), ctx_(ctx), column_(column), buf_() {
    GRN_TEXT_INIT(&buf_, 0);
  }
};

grn_rc ColumnNode<Text>::evaluate(
  const Record *records, size_t num_records, Text *results) {
  GRN_BULK_REWIND(&buf_);
  size_t offset = 0;
  for (size_t i = 0; i < num_records; i++) {
    grn_obj_get_value(ctx_, column_, records[i].id, &buf_);
    if (ctx_->rc != GRN_SUCCESS) {
      return ctx_->rc;
    }
    size_t next_offset = GRN_TEXT_LEN(&buf_);
    results[i].raw.size = next_offset - offset;
    offset = next_offset;
  }
  char *ptr = GRN_TEXT_VALUE(&buf_);
  for (size_t i = 0; i < num_records; i++) {
    results[i].raw.ptr = ptr;
    ptr += results[i].raw.size;
  }
  return GRN_SUCCESS;
}

// -- ColumnNode<GeoPoint> --

template <>
class ColumnNode<GeoPoint> : public TypedNode<GeoPoint> {
 public:
  ~ColumnNode() {}

  static grn_rc open(grn_ctx *ctx, grn_obj *column, ExpressionNode **node) {
    ColumnNode *new_node = new (std::nothrow) ColumnNode(ctx, column);
    if (!new_node) {
      return GRN_NO_MEMORY_AVAILABLE;
    }
    *node = new_node;
    return GRN_SUCCESS;
  }

  ExpressionNodeType type() const {
    return GRN_EGN_COLUMN_NODE;
  }

  grn_rc evaluate(
    const Record *records, size_t num_records, GeoPoint *results);

 private:
  grn_ctx *ctx_;
  grn_obj *column_;

  ColumnNode(grn_ctx *ctx, grn_obj *column)
    : TypedNode<GeoPoint>(), ctx_(ctx), column_(column) {}
};

grn_rc ColumnNode<GeoPoint>::evaluate(
  const Record *records, size_t num_records, GeoPoint *results) {
  grn_obj value;
  GRN_WGS84_GEO_POINT_INIT(&value, 0);
  for (size_t i = 0; i < num_records; i++) {
    GRN_BULK_REWIND(&value);
    grn_obj_get_value(ctx_, column_, records[i].id, &value);
    GRN_GEO_POINT_VALUE(
      &value, results[i].raw.latitude, results[i].raw.longitude);
  }
  GRN_OBJ_FIN(ctx_, &value);
  return GRN_SUCCESS;
}

// -- OperatorNode --

template <typename T>
class OperatorNode : public TypedNode<T> {
 public:
  OperatorNode() : TypedNode<T>() {}
  virtual ~OperatorNode() {}

  ExpressionNodeType type() const {
    return GRN_EGN_OPERATOR_NODE;
  }
};

template <typename T>
grn_rc operator_node_fill_arg_values(
  const Record *records, size_t num_records,
  TypedNode<T> *arg, std::vector<T> *arg_values) {
  size_t old_size = arg_values->size();
  if (old_size < num_records) try {
    arg_values->resize(num_records);
  } catch (const std::bad_alloc &) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  switch (arg->type()) {
    case GRN_EGN_CONSTANT_NODE: {
      if (old_size < num_records) {
        return arg->evaluate(records + old_size, num_records - old_size,
          &*arg_values->begin() + old_size);
      }
      return GRN_SUCCESS;
    }
    default: {
      return arg->evaluate(records, num_records, &*arg_values->begin());
    }
  }
}

// --- UnaryNode ---

template <typename T, typename U>
class UnaryNode : public OperatorNode<T> {
 public:
  explicit UnaryNode(ExpressionNode *arg)
    : OperatorNode<T>(), arg_(static_cast<TypedNode<U> *>(arg)),
      arg_values_() {}
  virtual ~UnaryNode() {
    delete arg_;
  }

 protected:
  TypedNode<U> *arg_;
  std::vector<U> arg_values_;

  grn_rc fill_arg_values(const Record *records, size_t num_records) {
    return operator_node_fill_arg_values(
      records, num_records, arg_, &arg_values_);
  }
};

// ---- LogicalNotNode ----

class LogicalNotNode : public UnaryNode<Bool, Bool> {
 public:
  ~LogicalNotNode() {}

  static grn_rc open(ExpressionNode *arg, ExpressionNode **node) {
    LogicalNotNode *new_node = new (std::nothrow) LogicalNotNode(arg);
    if (!new_node) {
      return GRN_NO_MEMORY_AVAILABLE;
    }
    *node = new_node;
    return GRN_SUCCESS;
  }

  grn_rc filter(
    Record *input, size_t input_size,
    Record *output, size_t *output_size);

  grn_rc evaluate(
    const Record *records, size_t num_records, Bool *results);

 private:
  std::vector<Record> temp_records_;

  explicit LogicalNotNode(ExpressionNode *arg)
    : UnaryNode<Bool, Bool>(arg), temp_records_() {}
};

grn_rc LogicalNotNode::filter(
  Record *input, size_t input_size,
  Record *output, size_t *output_size) {
  if (temp_records_.size() <= input_size) {
    try {
      temp_records_.resize(input_size + 1);
      temp_records_[input_size].id = GRN_ID_NIL;
    } catch (const std::bad_alloc &) {
      return GRN_NO_MEMORY_AVAILABLE;
    }
  }
  size_t temp_size;
  grn_rc rc =
    arg_->filter(input, input_size, &*temp_records_.begin(), &temp_size);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  if (temp_size == 0) {
    *output_size = 0;
    return GRN_SUCCESS;
  }

  size_t count = 0;
  for (size_t i = 0; i < input_size; ++i) {
    if (input[i].id != temp_records_[i - count].id) {
      output[count] = input[i];
      ++count;
    }
  }
  *output_size = count;
  return GRN_SUCCESS;
}

grn_rc LogicalNotNode::evaluate(
  const Record *records, size_t num_records, Bool *results) {
  grn_rc rc = arg_->evaluate(records, num_records, results);
  if (rc == GRN_SUCCESS) {
    for (size_t i = 0; i < num_records; ++i) {
      results[i] = Bool(results[i].raw != GRN_TRUE);
    }
  }
  return rc;
}

// --- BinaryNode ---

template <typename T, typename U, typename V>
class BinaryNode : public OperatorNode<T> {
 public:
  BinaryNode(ExpressionNode *arg1, ExpressionNode *arg2)
    : OperatorNode<T>(),
      arg1_(static_cast<TypedNode<U> *>(arg1)),
      arg2_(static_cast<TypedNode<V> *>(arg2)),
      arg1_values_(), arg2_values_() {}
  virtual ~BinaryNode() {
    delete arg1_;
    delete arg2_;
  }

 protected:
  TypedNode<U> *arg1_;
  TypedNode<V> *arg2_;
  std::vector<U> arg1_values_;
  std::vector<V> arg2_values_;

  grn_rc fill_arg1_values(const Record *records, size_t num_records) {
    return operator_node_fill_arg_values(
      records, num_records, arg1_, &arg1_values_);
  }
  grn_rc fill_arg2_values(const Record *records, size_t num_records) {
    return operator_node_fill_arg_values(
      records, num_records, arg2_, &arg2_values_);
  }
};

// ---- LogicalAndNode ----

class LogicalAndNode : public BinaryNode<Bool, Bool, Bool> {
 public:
  ~LogicalAndNode() {}

  static grn_rc open(
    ExpressionNode *arg1, ExpressionNode *arg2, ExpressionNode **node) {
    LogicalAndNode *new_node = new (std::nothrow) LogicalAndNode(arg1, arg2);
    if (!new_node) {
      return GRN_NO_MEMORY_AVAILABLE;
    }
    *node = new_node;
    return GRN_SUCCESS;
  }

  grn_rc filter(
    Record *input, size_t input_size,
    Record *output, size_t *output_size) {
    grn_rc rc = arg1_->filter(input, input_size, output, output_size);
    if (rc == GRN_SUCCESS) {
      rc = arg2_->filter(output, *output_size, output, output_size);
    }
    return rc;
  }

  grn_rc evaluate(
    const Record *records, size_t num_records, Bool *results);

 private:
  std::vector<Record> temp_records_;

  LogicalAndNode(ExpressionNode *arg1, ExpressionNode *arg2)
    : BinaryNode<Bool, Bool, Bool>(arg1, arg2), temp_records_() {}
};

grn_rc LogicalAndNode::evaluate(
  const Record *records, size_t num_records, Bool *results) {
  // Evaluate "arg1" for all the records.
  // Then, evaluate "arg2" for non-false records.
  grn_rc rc = arg1_->evaluate(records, num_records, results);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  if (temp_records_.size() < num_records) try {
    temp_records_.resize(num_records);
  } catch (const std::bad_alloc &) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  size_t count = 0;
  for (size_t i = 0; i < num_records; ++i) {
    if (results[i].raw == GRN_TRUE) {
      temp_records_[count] = records[i];
      ++count;
    }
  }
  if (count == 0) {
    // Nothing to do.
    return GRN_SUCCESS;
  }
  rc = fill_arg2_values(&*temp_records_.begin(), count);
  if (rc != GRN_SUCCESS) {
    return rc;
  }

  // Merge the evaluation results.
  count = 0;
  for (size_t i = 0; i < num_records; ++i) {
    if (results[i].raw == GRN_TRUE) {
      results[i] = arg2_values_[count];
      ++count;
    }
  }
  return GRN_SUCCESS;
}

// ---- LogicalOrNode ----

class LogicalOrNode : public BinaryNode<Bool, Bool, Bool> {
 public:
  ~LogicalOrNode() {}

  static grn_rc open(
    ExpressionNode *arg1, ExpressionNode *arg2, ExpressionNode **node) {
    LogicalOrNode *new_node = new (std::nothrow) LogicalOrNode(arg1, arg2);
    if (!new_node) {
      return GRN_NO_MEMORY_AVAILABLE;
    }
    *node = new_node;
    return GRN_SUCCESS;
  }

  grn_rc filter(
    Record *input, size_t input_size,
    Record *output, size_t *output_size);

  grn_rc evaluate(
    const Record *records, size_t num_records, Bool *results);

 private:
  std::vector<Record> temp_records_;

  LogicalOrNode(ExpressionNode *arg1, ExpressionNode *arg2)
    : BinaryNode<Bool, Bool, Bool>(arg1, arg2), temp_records_() {}
};

grn_rc LogicalOrNode::filter(
  Record *input, size_t input_size,
  Record *output, size_t *output_size) {
  // Evaluate "arg1" for all the records.
  // Then, evaluate "arg2" for non-true records.
  grn_rc rc = fill_arg1_values(input, input_size);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  if (temp_records_.size() < input_size) try {
    temp_records_.resize(input_size);
  } catch (const std::bad_alloc &) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  size_t count = 0;
  for (size_t i = 0; i < input_size; ++i) {
    if (arg1_values_[i].raw == GRN_FALSE) {
      temp_records_[count] = input[i];
      ++count;
    }
  }
  if (count == 0) {
    if (input != output) {
      for (size_t i = 0; i < input_size; ++i) {
        output[i] = input[i];
      }
    }
    *output_size = input_size;
    return GRN_SUCCESS;
  }
  rc = fill_arg2_values(&*temp_records_.begin(), count);
  if (rc != GRN_SUCCESS) {
    return rc;
  }

  // Merge the evaluation results.
  count = 0;
  size_t output_count = 0;
  for (size_t i = 0; i < input_size; ++i) {
    if (arg1_values_[i].raw == GRN_TRUE) {
      output[output_count] = input[i];
      ++output_count;
    } else {
      if (arg2_values_[count].raw == GRN_TRUE) {
        output[output_count] = input[i];
        ++output_count;
      }
      ++count;
    }
  }
  *output_size = output_count;
  return GRN_SUCCESS;
}

grn_rc LogicalOrNode::evaluate(
  const Record *records, size_t num_records, Bool *results) {
  // Evaluate "arg1" for all the records.
  // Then, evaluate "arg2" for non-true records.
  grn_rc rc = arg1_->evaluate(records, num_records, results);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  if (temp_records_.size() < num_records) try {
    temp_records_.resize(num_records);
  } catch (const std::bad_alloc &) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  size_t count = 0;
  for (size_t i = 0; i < num_records; ++i) {
    if (results[i].raw == GRN_FALSE) {
      temp_records_[count] = records[i];
      ++count;
    }
  }
  if (count == 0) {
    // Nothing to do.
    return GRN_SUCCESS;
  }
  rc = fill_arg2_values(&*temp_records_.begin(), count);
  if (rc != GRN_SUCCESS) {
    return rc;
  }

  // Merge the evaluation results.
  count = 0;
  for (size_t i = 0; i < num_records; ++i) {
    if (results[i].raw == GRN_FALSE) {
      results[i] = arg2_values_[count];
      ++count;
    }
  }
  return GRN_SUCCESS;
}

// -- GenericBinaryNode --

template <typename T,
          typename U = typename T::Value,
          typename V = typename T::Arg1,
          typename W = typename T::Arg2>
class GenericBinaryNode : public BinaryNode<U, V, W> {
 public:
  GenericBinaryNode(ExpressionNode *arg1, ExpressionNode *arg2)
    : BinaryNode<U, V, W>(arg1, arg2), operator_() {}
  ~GenericBinaryNode() {}

  grn_rc evaluate(
    const Record *records, size_t num_records, Bool *results);

 private:
  T operator_;
};

template <typename T, typename U, typename V, typename W>
grn_rc GenericBinaryNode<T, U, V, W>::evaluate(
    const Record *records, size_t num_records, Bool *results) {
  grn_rc rc = this->fill_arg1_values(records, num_records);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  rc = this->fill_arg2_values(records, num_records);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  for (size_t i = 0; i < num_records; ++i) {
    results[i] = operator_(this->arg1_values_[i], this->arg2_values_[i]);
  }
  return GRN_SUCCESS;
}

template <typename T, typename V, typename W>
class GenericBinaryNode<T, Bool, V, W> : public BinaryNode<Bool, V, W> {
 public:
  GenericBinaryNode(ExpressionNode *arg1, ExpressionNode *arg2)
    : BinaryNode<Bool, V, W>(arg1, arg2), operator_() {}
  ~GenericBinaryNode() {}

  grn_rc filter(
    Record *input, size_t input_size,
    Record *output, size_t *output_size);

  grn_rc evaluate(
    const Record *records, size_t num_records, Bool *results);

 private:
  T operator_;
};

template <typename T, typename V, typename W>
grn_rc GenericBinaryNode<T, Bool, V, W>::filter(
    Record *input, size_t input_size,
    Record *output, size_t *output_size) {
  grn_rc rc = this->fill_arg1_values(input, input_size);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  rc = this->fill_arg2_values(input, input_size);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  size_t count = 0;
  for (size_t i = 0; i < input_size; ++i) {
    if (operator_(this->arg1_values_[i], this->arg2_values_[i]).raw ==
      GRN_TRUE) {
      output[count] = input[i];
      ++count;
    }
  }
  *output_size = count;
  return GRN_SUCCESS;
}

template <typename T, typename V, typename W>
grn_rc GenericBinaryNode<T, Bool, V, W>::evaluate(
  const Record *records, size_t num_records, Bool *results) {
  grn_rc rc = this->fill_arg1_values(records, num_records);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  rc = this->fill_arg2_values(records, num_records);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  for (size_t i = 0; i < num_records; ++i) {
    results[i] = operator_(this->arg1_values_[i], this->arg2_values_[i]);
  }
  return GRN_SUCCESS;
}

// ----- EqualNode -----

template <typename T>
struct EqualOperator {
  typedef Bool Value;
  typedef T Arg1;
  typedef T Arg2;
  Value operator()(const Arg1 &arg1, const Arg2 &arg2) const {
    return Bool(arg1 == arg2);
  }
};

template <typename T>
grn_rc equal_node_open(EqualOperator<T> op,
  ExpressionNode *arg1, ExpressionNode *arg2, ExpressionNode **node) {
  GenericBinaryNode<EqualOperator<T> > *new_node =
    new (std::nothrow) GenericBinaryNode<EqualOperator<T> >(arg1, arg2);
  if (!new_node) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  *node = new_node;
  return GRN_SUCCESS;
}

// ----- NotEqualNode -----

template <typename T>
struct NotEqualOperator {
  typedef Bool Value;
  typedef T Arg1;
  typedef T Arg2;
  Value operator()(const Arg1 &arg1, const Arg2 &arg2) const {
    return Bool(arg1 != arg2);
  }
};

template <typename T>
grn_rc not_equal_node_open(NotEqualOperator<T> op,
  ExpressionNode *arg1, ExpressionNode *arg2, ExpressionNode **node) {
  GenericBinaryNode<NotEqualOperator<T> > *new_node =
    new (std::nothrow) GenericBinaryNode<NotEqualOperator<T> >(arg1, arg2);
  if (!new_node) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  *node = new_node;
  return GRN_SUCCESS;
}

// ----- LessNode -----

template <typename T>
struct LessOperator {
  typedef Bool Value;
  typedef T Arg1;
  typedef T Arg2;
  Value operator()(const Arg1 &arg1, const Arg2 &arg2) const {
    return Bool(arg1 < arg2);
  }
};

template <typename T>
grn_rc less_node_open(LessOperator<T> op,
  ExpressionNode *arg1, ExpressionNode *arg2, ExpressionNode **node) {
  GenericBinaryNode<LessOperator<T> > *new_node =
    new (std::nothrow) GenericBinaryNode<LessOperator<T> >(arg1, arg2);
  if (!new_node) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  *node = new_node;
  return GRN_SUCCESS;
}

// ----- LessEqualNode -----

template <typename T>
struct LessEqualOperator {
  typedef Bool Value;
  typedef T Arg1;
  typedef T Arg2;
  Value operator()(const Arg1 &arg1, const Arg2 &arg2) const {
    return Bool(arg1 < arg2);
  }
};

template <typename T>
grn_rc less_equal_node_open(LessEqualOperator<T> op,
  ExpressionNode *arg1, ExpressionNode *arg2, ExpressionNode **node) {
  GenericBinaryNode<LessEqualOperator<T> > *new_node =
    new (std::nothrow) GenericBinaryNode<LessEqualOperator<T> >(arg1, arg2);
  if (!new_node) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  *node = new_node;
  return GRN_SUCCESS;
}

// ----- GreaterNode -----

template <typename T>
struct GreaterOperator {
  typedef Bool Value;
  typedef T Arg1;
  typedef T Arg2;
  Value operator()(const Arg1 &arg1, const Arg2 &arg2) const {
    return Bool(arg1 < arg2);
  }
};

template <typename T>
grn_rc greater_node_open(GreaterOperator<T> op,
  ExpressionNode *arg1, ExpressionNode *arg2, ExpressionNode **node) {
  GenericBinaryNode<GreaterOperator<T> > *new_node =
    new (std::nothrow) GenericBinaryNode<GreaterOperator<T> >(arg1, arg2);
  if (!new_node) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  *node = new_node;
  return GRN_SUCCESS;
}

// ----- GreaterEqualNode -----

template <typename T>
struct GreaterEqualOperator {
  typedef Bool Value;
  typedef T Arg1;
  typedef T Arg2;
  Value operator()(const Arg1 &arg1, const Arg2 &arg2) const {
    return Bool(arg1 < arg2);
  }
};

template <typename T>
grn_rc greater_equal_node_open(GreaterEqualOperator<T> op,
  ExpressionNode *arg1, ExpressionNode *arg2, ExpressionNode **node) {
  GenericBinaryNode<GreaterEqualOperator<T> > *new_node =
    new (std::nothrow) GenericBinaryNode<GreaterEqualOperator<T> >(arg1, arg2);
  if (!new_node) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  *node = new_node;
  return GRN_SUCCESS;
}

// -- ExpressionToken --

enum ExpressionTokenType {
  DUMMY_TOKEN,
  CONSTANT_TOKEN,
  NAME_TOKEN,
  UNARY_OPERATOR_TOKEN,
  BINARY_OPERATOR_TOKEN,
  DEREFERENCE_TOKEN,
  BRACKET_TOKEN
};

enum ExpressionBracketType {
  LEFT_ROUND_BRACKET,
  RIGHT_ROUND_BRACKET,
  LEFT_SQUARE_BRACKET,
  RIGHT_SQUARE_BRACKET
};

// TODO: std::string should not be used.
class ExpressionToken {
 public:
  ExpressionToken() : string_(), type_(DUMMY_TOKEN), dummy_(0), priority_(0) {}
  ExpressionToken(const std::string &string, ExpressionTokenType token_type)
    : string_(string), type_(token_type), dummy_(0), priority_(0) {}
  ExpressionToken(const std::string &string,
    ExpressionBracketType bracket_type)
      : string_(string), type_(BRACKET_TOKEN), bracket_type_(bracket_type),
        priority_(0) {}
  ExpressionToken(const std::string &string, OperatorType operator_type)
      : string_(string), type_(get_operator_token_type(operator_type)),
        operator_type_(operator_type),
        priority_(get_operator_priority(operator_type)) {}

  const std::string &string() const {
    return string_;
  }
  ExpressionTokenType type() const {
    return type_;
  }
  ExpressionBracketType bracket_type() const {
    return bracket_type_;
  }
  OperatorType operator_type() const {
    return operator_type_;
  }
  int priority() const {
    return priority_;
  }

 private:
  std::string string_;
  ExpressionTokenType type_;
  union {
    int dummy_;
    ExpressionBracketType bracket_type_;
    OperatorType operator_type_;
  };
  int priority_;

  static ExpressionTokenType get_operator_token_type(
    OperatorType operator_type);
  static int get_operator_priority(OperatorType operator_type);
};

ExpressionTokenType ExpressionToken::get_operator_token_type(
  OperatorType operator_type) {
  switch (operator_type) {
    case GRN_OP_NOT: {
      return UNARY_OPERATOR_TOKEN;
    }
    case GRN_OP_AND:
    case GRN_OP_OR:
    case GRN_OP_EQUAL:
    case GRN_OP_NOT_EQUAL:
    case GRN_OP_LESS:
    case GRN_OP_LESS_EQUAL:
    case GRN_OP_GREATER:
    case GRN_OP_GREATER_EQUAL: {
      return BINARY_OPERATOR_TOKEN;
    }
    default: {
      // TODO: ERROR_TOKEN or something should be used...?
      //       Or, default should be removed?
      return DUMMY_TOKEN;
    }
  }
}

int ExpressionToken::get_operator_priority(
  OperatorType operator_type) {
  switch (operator_type) {
    case GRN_OP_NOT: {
//    case GRN_OP_BITWISE_NOT:
//    case GRN_OP_POSITIVE:
//    case GRN_OP_NEGATIVE:
//    case GRN_OP_TO_INT:
//    case GRN_OP_TO_FLOAT: {
      return 3;
    }
    case GRN_OP_AND: {
      return 13;
    }
    case GRN_OP_OR: {
      return 14;
    }
    case GRN_OP_EQUAL:
    case GRN_OP_NOT_EQUAL: {
      return 9;
    }
    case GRN_OP_LESS:
    case GRN_OP_LESS_EQUAL:
    case GRN_OP_GREATER:
    case GRN_OP_GREATER_EQUAL: {
      return 8;
    }
//    case GRN_OP_BITWISE_AND: {
//      return 10;
//    }
//    case GRN_OP_BITWISE_OR: {
//      return 12;
//    }
//    case GRN_OP_BITWISE_XOR: {
//      return 11;
//    }
//    case GRN_OP_PLUS:
//    case GRN_OP_MINUS: {
//      return 6;
//    }
//    case GRN_OP_MULTIPLICATION:
//    case GRN_OP_DIVISION:
//    case GRN_OP_MODULUS: {
//      return 5;
//    }
//    case GRN_OP_STARTS_WITH:
//    case GRN_OP_ENDS_WITH:
//    case GRN_OP_CONTAINS: {
//      return 7;
//    }
//    case GRN_OP_SUBSCRIPT: {
//      return 2;
//    }
    default: {
      return 100;
    }
  }
}

// -- ExpressionParser --

class ExpressionParser {
 public:
  static grn_rc parse(grn_ctx *ctx, grn_obj *table,
    const char *query, size_t query_size, Expression **expression);

 private:
  grn_ctx *ctx_;
  grn_obj *table_;
  std::vector<ExpressionToken> tokens_;
  std::vector<ExpressionToken> stack_;
  Expression *expression_;

  ExpressionParser(grn_ctx *ctx, grn_obj *table)
    : ctx_(ctx), table_(table), tokens_(), stack_(), expression_(NULL) {}
  ~ExpressionParser() {
    delete expression_;
  }

  grn_rc tokenize(const char *query, size_t query_size);
  grn_rc compose();
  grn_rc push_token(const ExpressionToken &token);
};

grn_rc ExpressionParser::parse(grn_ctx *ctx, grn_obj *table,
  const char *query, size_t query_size, Expression **expression) {
  ExpressionParser *parser = new (std::nothrow) ExpressionParser(ctx, table);
  if (!parser) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  grn_rc rc = parser->tokenize(query, query_size);
  if (rc == GRN_SUCCESS) {
    rc = parser->compose();
    if (rc == GRN_SUCCESS) {
      *expression = parser->expression_;
      parser->expression_ = NULL;
    }
  }
  delete parser;
  return rc;
}

grn_rc ExpressionParser::tokenize(const char *query, size_t query_size) {
  const char *rest = query;
  size_t rest_size = query_size;
  while (rest_size != 0) {
    // Ignore white-space characters.
    size_t pos;
    for (pos = 0; pos < rest_size; ++pos) {
      if (!std::isspace(static_cast<uint8_t>(rest[pos]))) {
        break;
      }
    }
    rest += pos;
    rest_size -= pos;
    switch (rest[0]) {
      case '!': {
        if ((rest_size >= 2) && (rest[1] == '=')) {
          tokens_.push_back(ExpressionToken("!=", GRN_OP_NOT_EQUAL));
          rest += 2;
          rest_size -= 2;
        } else {
          tokens_.push_back(ExpressionToken("!", GRN_OP_NOT));
          ++rest;
          --rest_size;
        }
        break;
      }
//      case '~': {
//        tokens_.push_back(ExpressionToken("~", GRN_OP_BITWISE_NOT));
//        rest = rest.substring(1);
//        break;
//      }
      case '=': {
        if ((rest_size >= 2) && (rest[1] == '=')) {
          tokens_.push_back(ExpressionToken("==", GRN_OP_EQUAL));
          rest += 2;
          rest_size -= 2;
        } else {
          return GRN_INVALID_ARGUMENT;
        }
        break;
      }
      case '<': {
        if ((rest_size >= 2) && (rest[1] == '=')) {
          tokens_.push_back(ExpressionToken("<=", GRN_OP_LESS_EQUAL));
          rest += 2;
          rest_size -= 2;
        } else {
          tokens_.push_back(ExpressionToken("<", GRN_OP_LESS));
          ++rest;
          --rest_size;
        }
        break;
      }
      case '>': {
        if ((rest_size >= 2) && (rest[1] == '=')) {
          tokens_.push_back(ExpressionToken(">=", GRN_OP_GREATER_EQUAL));
          rest += 2;
          rest_size -= 2;
        } else {
          tokens_.push_back(ExpressionToken(">", GRN_OP_GREATER));
          ++rest;
          --rest_size;
        }
        break;
      }
      case '&': {
        if ((rest_size >= 2) && (rest[1] == '&')) {
          tokens_.push_back(ExpressionToken("&&", GRN_OP_AND));
          rest += 2;
          rest_size -= 2;
        } else {
//          tokens_.push_back(ExpressionToken("&", GRN_OP_BITWISE_AND));
//          ++rest;
//          --rest_size;
          return GRN_INVALID_ARGUMENT;
        }
        break;
      }
      case '|': {
        if ((rest_size >= 2) && (rest[1] == '|')) {
          tokens_.push_back(ExpressionToken("||", GRN_OP_OR));
          rest += 2;
          rest_size -= 2;
        } else {
//          tokens_.push_back(ExpressionToken("|", GRN_OP_BITWISE_OR));
//          ++rest;
//          --rest_size;
          return GRN_INVALID_ARGUMENT;
        }
        break;
      }
//      case '^': {
//        tokens_.push_back(ExpressionToken("^", GRN_OP_BITWISE_XOR));
//        rest = rest.substring(1);
//        break;
//      }
//      case '+': {
//        tokens_.push_back(ExpressionToken("+", GRN_OP_PLUS));
//        rest = rest.substring(1);
//        break;
//      }
//      case '-': {
//        tokens_.push_back(ExpressionToken("-", GRN_OP_MINUS));
//        rest = rest.substring(1);
//        break;
//      }
//      case '*': {
//        tokens_.push_back(ExpressionToken("*", GRN_OP_MULTIPLICATION));
//        rest = rest.substring(1);
//        break;
//      }
//      case '/': {
//        tokens_.push_back(ExpressionToken("/", GRN_OP_DIVISION));
//        rest = rest.substring(1);
//        break;
//      }
//      case '%': {
//        tokens_.push_back(ExpressionToken("%", GRN_OP_MODULUS));
//        rest = rest.substring(1);
//        break;
//      }
//      case '@': {
//        if ((rest_size >= 2) && (rest[1] == '^')) {
//          tokens_.push_back(ExpressionToken("@^", GRN_OP_STARTS_WITH));
//          rest = rest.substring(2);
//        } else if ((rest_size >= 2) && (rest[1] == '$')) {
//          tokens_.push_back(ExpressionToken("@$", GRN_OP_ENDS_WITH));
//          rest = rest.substring(2);
//        } else {
//          tokens_.push_back(ExpressionToken("@", GRN_OP_CONTAINS));
//          rest = rest.substring(1);
//        }
//        break;
//      }
//      case '.': {
//        tokens_.push_back(ExpressionToken(".", DEREFERENCE_TOKEN));
//        rest = rest.substring(1);
//        break;
//      }
      case '(': {
        tokens_.push_back(ExpressionToken("(", LEFT_ROUND_BRACKET));
        ++rest;
        --rest_size;
        break;
      }
      case ')': {
        tokens_.push_back(ExpressionToken(")", RIGHT_ROUND_BRACKET));
        ++rest;
        --rest_size;
        break;
      }
//      case '[': {
//        tokens_.push_back(ExpressionToken("[", LEFT_SQUARE_BRACKET));
//        rest = rest.substring(1);
//        break;
//      }
//      case ']': {
//        tokens_.push_back(ExpressionToken("]", RIGHT_SQUARE_BRACKET));
//        rest = rest.substring(1);
//        break;
//      }
      case '"': {
        for (pos = 1; pos < rest_size; ++pos) {
          if (rest[pos] == '\\') {
            if (pos == rest_size) {
              break;
            }
            ++pos;
          } else if (rest[pos] == '"') {
            break;
          }
        }
        if (pos == rest_size) {
          return GRN_INVALID_ARGUMENT;
        }
        tokens_.push_back(
          ExpressionToken(std::string(rest + 1, pos - 1), CONSTANT_TOKEN));
        rest += pos + 1;
        rest_size -= pos + 1;
        break;
      }
      case '0' ... '9': {
        // TODO: Improve this.
        for (pos = 1; pos < rest_size; ++pos) {
          if (!std::isdigit(static_cast<uint8_t>(rest[pos]))) {
            break;
          }
        }
        tokens_.push_back(
          ExpressionToken(std::string(rest, pos), CONSTANT_TOKEN));
        rest += pos;
        rest_size -= pos;
        break;
      }
      case '_':
      case 'A' ... 'Z':
      case 'a' ... 'z': {
        // TODO: Improve this.
        for (pos = 1; pos < rest_size; ++pos) {
          if ((rest[pos] != '_') && (!std::isalnum(rest[pos]))) {
            break;
          }
        }
        std::string token(rest, pos);
        if ((token == "true") || (token == "false")) {
          tokens_.push_back(ExpressionToken(token, CONSTANT_TOKEN));
        } else {
          tokens_.push_back(ExpressionToken(token, NAME_TOKEN));
        }
        rest += pos;
        rest_size -= pos;
        break;
      }
      default: {
        return GRN_INVALID_ARGUMENT;
      }
    }
  }
  return GRN_SUCCESS;
}

grn_rc ExpressionParser::compose() {
  if (tokens_.size() == 0) {
    return GRN_INVALID_ARGUMENT;
  }
  expression_ = new (std::nothrow) Expression(ctx_, table_);
  grn_rc rc = push_token(ExpressionToken("(", LEFT_ROUND_BRACKET));
  if (rc == GRN_SUCCESS) {
    for (size_t i = 0; i < tokens_.size(); ++i) {
      rc = push_token(tokens_[i]);
      if (rc != GRN_SUCCESS) {
        break;
      }
    }
    if (rc == GRN_SUCCESS) {
      rc = push_token(ExpressionToken(")", RIGHT_ROUND_BRACKET));
    }
  }
  return rc;
}

grn_rc ExpressionParser::push_token(const ExpressionToken &token) {
  grn_rc rc = GRN_SUCCESS;
  switch (token.type()) {
    case DUMMY_TOKEN: {
      if ((stack_.size() != 0) && (stack_.back().type() == DUMMY_TOKEN)) {
        return GRN_INVALID_ARGUMENT;
      }
      stack_.push_back(token);
      break;
    }
    case CONSTANT_TOKEN: {
      grn_obj obj;
      const std::string string = token.string();
      if (std::isdigit(static_cast<uint8_t>(string[0]))) {
        if (string.find_first_of('.') == string.npos) {
          GRN_INT64_INIT(&obj, 0);
          GRN_INT64_SET(ctx_, &obj, strtoll(string.c_str(), NULL, 10));
        } else {
          GRN_FLOAT_INIT(&obj, 0);
          GRN_FLOAT_SET(ctx_, &obj, strtod(string.c_str(), NULL));
        }
      } else if (string == "true") {
        GRN_BOOL_INIT(&obj, 0);
        GRN_BOOL_SET(ctx_, &obj, GRN_TRUE);
      } else if (string == "false") {
        GRN_BOOL_INIT(&obj, 0);
        GRN_BOOL_SET(ctx_, &obj, GRN_FALSE);
      } else {
        GRN_TEXT_INIT(&obj, 0);
        GRN_TEXT_SET(ctx_, &obj, string.data(), string.size());
      }
      rc = push_token(ExpressionToken(string, DUMMY_TOKEN));
      if (rc == GRN_SUCCESS) {
        rc = expression_->push_object(&obj);
      }
      GRN_OBJ_FIN(ctx_, &obj);
      break;
    }
    case NAME_TOKEN: {
      rc = push_token(ExpressionToken(token.string(), DUMMY_TOKEN));
      if (rc == GRN_SUCCESS) {
        grn_obj *column = grn_obj_column(
          ctx_, table_, token.string().data(), token.string().size());
        rc = expression_->push_object(column);
      }
      break;
    }
    case UNARY_OPERATOR_TOKEN: {
      if ((stack_.size() != 0) && (stack_.back().type() == DUMMY_TOKEN)) {
        // A unary operator must not follow an operand.
        return GRN_INVALID_ARGUMENT;
      }
      stack_.push_back(token);
      break;
    }
    case BINARY_OPERATOR_TOKEN: {
      if ((stack_.size() == 0) || (stack_.back().type() != DUMMY_TOKEN)) {
        // A binary operator must follow an operand.
        return GRN_INVALID_ARGUMENT;
      }
      // Apply previous operators if those are prior to the new operator.
      while (stack_.size() >= 2) {
        ExpressionToken operator_token = stack_[stack_.size() - 2];
//        if (operator_token.type() == DEREFERENCE_TOKEN) {
//          expression_->end_subexpression();
//          stack_.pop_back();
//          stack_.pop_back();
//          push_token(ExpressionToken("", DUMMY_TOKEN));
//        } else if (operator_token.type() == UNARY_OPERATOR_TOKEN) {
        if (operator_token.type() == UNARY_OPERATOR_TOKEN) {
          rc = expression_->push_operator(operator_token.operator_type());
          if (rc == GRN_SUCCESS) {
            stack_.pop_back();
            stack_.pop_back();
            rc = push_token(ExpressionToken("", DUMMY_TOKEN));
          }
        } else if ((operator_token.type() == BINARY_OPERATOR_TOKEN) &&
                   (operator_token.priority() <= token.priority())) {
          rc = expression_->push_operator(operator_token.operator_type());
          if (rc == GRN_SUCCESS) {
            stack_.pop_back();
            stack_.pop_back();
            stack_.pop_back();
            rc = push_token(ExpressionToken("", DUMMY_TOKEN));
          }
        } else {
          break;
        }
        if (rc != GRN_SUCCESS) {
          return rc;
        }
      }
      stack_.push_back(token);
      break;
    }
//    case DEREFERENCE_TOKEN: {
//      builder_->begin_subexpression();
//      stack_.pop_back();
//      stack_.push_back(token);
//      break;
//    }
    case BRACKET_TOKEN: {
      if (token.bracket_type() == LEFT_ROUND_BRACKET) {
        // A left round bracket must not follow a dummy.
        if ((stack_.size() != 0) && (stack_.back().type() == DUMMY_TOKEN)) {
          return GRN_INVALID_ARGUMENT;
        }
        stack_.push_back(token);
      } else if (token.bracket_type() == RIGHT_ROUND_BRACKET) {
        // A right round bracket must follow a dummy.
        // A left round bracket must exist before a right round bracket.
        if ((stack_.size() < 2) || (stack_.back().type() != DUMMY_TOKEN)) {
          return GRN_INVALID_ARGUMENT;
        }
        // Apply operators in brackets.
        while (stack_.size() >= 2) {
          ExpressionToken operator_token = stack_[stack_.size() - 2];
//          if (operator_token.type() == DEREFERENCE_TOKEN) {
//            rc = expression_->end_subexpression();
//            if (rc == GRN_SUCCESS) {
//              stack_.pop_back();
//              stack_.pop_back();
//              rc = push_token(ExpressionToken("", DUMMY_TOKEN));
//            }
//          } else if (operator_token.type() == UNARY_OPERATOR_TOKEN) {
          if (operator_token.type() == UNARY_OPERATOR_TOKEN) {
            rc = expression_->push_operator(operator_token.operator_type());
            if (rc == GRN_SUCCESS) {
              stack_.pop_back();
              stack_.pop_back();
              rc = push_token(ExpressionToken("", DUMMY_TOKEN));
            }
          } else if (operator_token.type() == BINARY_OPERATOR_TOKEN) {
            rc = expression_->push_operator(operator_token.operator_type());
            if (rc == GRN_SUCCESS) {
              stack_.pop_back();
              stack_.pop_back();
              stack_.pop_back();
              rc = push_token(ExpressionToken("", DUMMY_TOKEN));
            }
          } else {
            break;
          }
          if (rc != GRN_SUCCESS) {
            return rc;
          }
        }
        if ((stack_.size() < 2) ||
            (stack_[stack_.size() - 2].type() != BRACKET_TOKEN) ||
            (stack_[stack_.size() - 2].bracket_type() != LEFT_ROUND_BRACKET)) {
          return GRN_INVALID_ARGUMENT;
        }
        stack_[stack_.size() - 2] = stack_.back();
        stack_.pop_back();
//      } else if (token.bracket_type() == LEFT_SQUARE_BRACKET) {
//        // A left square bracket must follow a dummy.
//        if ((stack_.size() == 0) || (stack_.back().type() != DUMMY_TOKEN)) {
//          return GRN_INVALID_ARGUMENT;
//        }
//        stack_.push_back(token);
//      } else if (token.bracket_type() == RIGHT_SQUARE_BRACKET) {
//        // A right round bracket must follow a dummy.
//        // A left round bracket must exist before a right round bracket.
//        if ((stack_.size() < 2) || (stack_.back().type() != DUMMY_TOKEN)) {
//          return GRN_INVALID_ARGUMENT;
//        }
//        // Apply operators in bracket.
//        while (stack_.size() >= 2) {
//          ExpressionToken operator_token = stack_[stack_.size() - 2];
//          if (operator_token.type() == DEREFERENCE_TOKEN) {
//            builder_->end_subexpression();
//            stack_.pop_back();
//            stack_.pop_back();
//            push_token(ExpressionToken("", DUMMY_TOKEN));
//          } else if (operator_token.type() == UNARY_OPERATOR_TOKEN) {
//            builder_->push_operator(operator_token.operator_type());
//            stack_.pop_back();
//            stack_.pop_back();
//            push_token(ExpressionToken("", DUMMY_TOKEN));
//          } else if (operator_token.type() == BINARY_OPERATOR_TOKEN) {
//            builder_->push_operator(operator_token.operator_type());
//            stack_.pop_back();
//            stack_.pop_back();
//            stack_.pop_back();
//            push_token(ExpressionToken("", DUMMY_TOKEN));
//          } else {
//            break;
//          }
//        }
//        if ((stack_.size() < 2) ||
//            (stack_[stack_.size() - 2].type() != BRACKET_TOKEN) ||
//            (stack_[stack_.size() - 2].bracket_type() != LEFT_SQUARE_BRACKET)) {
//          return GRN_INVALID_ARGUMENT;
//        }
//        stack_.pop_back();
//        stack_.pop_back();
//        builder_->push_operator(GRNXX_SUBSCRIPT);
      } else {
        return GRN_INVALID_ARGUMENT;
      }
      break;
    }
    default: {
      return GRN_INVALID_ARGUMENT;
    }
  }
  return rc;
}

// -- Expression --

Expression::Expression(grn_ctx *ctx, grn_obj *table)
  : ctx_(ctx), table_(table), type_(GRN_EGN_INCOMPLETE),
    data_type_(GRN_DB_VOID), stack_() {}

Expression::~Expression() {
  for (size_t i = 0; i < stack_.size(); ++i) {
    delete stack_[i];
  }
}

grn_rc Expression::open(
  grn_ctx *ctx, grn_obj *table, Expression **expression) {
  if (!ctx || !grn_egn_is_table(table) || !expression) {
    return GRN_INVALID_ARGUMENT;
  }
  Expression *new_expression = new (std::nothrow) Expression(ctx, table);
  if (!new_expression) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  *expression = new_expression;
  return GRN_SUCCESS;
}

grn_rc Expression::parse(grn_ctx *ctx, grn_obj *table,
  const char *query, size_t query_size, Expression **expression) {
  if (!ctx || !grn_egn_is_table(table) ||
      !query || (query_size == 0) || !expression) {
    return GRN_INVALID_ARGUMENT;
  }
  return ExpressionParser::parse(ctx, table, query, query_size, expression);
}

grn_rc Expression::push_object(grn_obj *obj) {
  if (!obj) {
    return GRN_INVALID_ARGUMENT;
  }
  grn_rc rc = GRN_UNKNOWN_ERROR;
  switch (obj->header.type) {
    case GRN_BULK: {
      rc = push_bulk_object(obj);
      break;
    }
    case GRN_UVECTOR: {
      // FIXME: To be supported.
      return GRN_INVALID_ARGUMENT;
    }
    case GRN_VECTOR: {
      // FIXME: To be supported.
      return GRN_INVALID_ARGUMENT;
    }
    case GRN_ACCESSOR: {
      grn_accessor *accessor = (grn_accessor *)obj;
      switch (accessor->action) {
        case GRN_ACCESSOR_GET_ID: {
          ExpressionNode *node;
          rc = IDNode::open(&node);
          if (rc == GRN_SUCCESS) try {
            stack_.push_back(node);
          } catch (const std::bad_alloc &) {
            delete node;
            return GRN_NO_MEMORY_AVAILABLE;
          }
          break;
        }
        case GRN_ACCESSOR_GET_KEY: {
          // TODO: KeyNode should be provided for performance.
          ExpressionNode *node;
          grn_id range = grn_obj_get_range(ctx_, obj);
          switch (range) {
            case GRN_DB_BOOL: {
              rc = ColumnNode<Bool>::open(ctx_, obj, &node);
              break;
            }
            case GRN_DB_INT8:
            case GRN_DB_INT16:
            case GRN_DB_INT32:
            case GRN_DB_INT64:
            case GRN_DB_UINT8:
            case GRN_DB_UINT16:
            case GRN_DB_UINT32:
            case GRN_DB_UINT64: {
              rc = ColumnNode<Int>::open(ctx_, obj, &node);
              break;
            }
            case GRN_DB_FLOAT: {
              rc = ColumnNode<Float>::open(ctx_, obj, &node);
              break;
            }
            case GRN_DB_TIME: {
              rc = ColumnNode<Time>::open(ctx_, obj, &node);
              break;
            }
            case GRN_DB_TOKYO_GEO_POINT:
            case GRN_DB_WGS84_GEO_POINT: {
              rc = ColumnNode<GeoPoint>::open(ctx_, obj, &node);
              break;
            }
            default: {
              return GRN_INVALID_ARGUMENT;
            }
          }
          if (rc == GRN_SUCCESS) try {
            stack_.push_back(node);
          } catch (const std::bad_alloc &) {
            delete node;
            return GRN_NO_MEMORY_AVAILABLE;
          }
          break;
        }
        case GRN_ACCESSOR_GET_VALUE: {
          // TODO
          return GRN_INVALID_ARGUMENT;
        }
        case GRN_ACCESSOR_GET_SCORE: {
          ExpressionNode *node;
          rc = ScoreNode::open(&node);
          if (rc == GRN_SUCCESS) try {
            stack_.push_back(node);
          } catch (const std::bad_alloc &) {
            delete node;
            return GRN_NO_MEMORY_AVAILABLE;
          }
          break;
        }
        default: {
          return GRN_INVALID_ARGUMENT;
        }
      }
      break;
    }
    case GRN_COLUMN_FIX_SIZE:
    case GRN_COLUMN_VAR_SIZE: {
      rc = push_column_object(obj);
      break;
    }
    default: {
      return GRN_INVALID_ARGUMENT;
    }
  }
  if (rc == GRN_SUCCESS) {
    update_types();
  }
  return rc;
}

grn_rc Expression::push_operator(OperatorType operator_type) {
  grn_rc rc = GRN_UNKNOWN_ERROR;
  ExpressionNode *node;
  switch (operator_type) {
    case GRN_OP_NOT: {
      if (stack_.size() < 1) {
        return GRN_INVALID_FORMAT;
      }
      ExpressionNode *arg = stack_[stack_.size() - 1];
      rc = create_unary_node(operator_type, arg, &node);
      if (rc == GRN_SUCCESS) {
        stack_.resize(stack_.size() - 1);
      }
      break;
    }
    case GRN_OP_AND:
    case GRN_OP_OR:
    case GRN_OP_EQUAL:
    case GRN_OP_NOT_EQUAL:
    case GRN_OP_LESS:
    case GRN_OP_LESS_EQUAL:
    case GRN_OP_GREATER:
    case GRN_OP_GREATER_EQUAL: {
      if (stack_.size() < 2) {
        return GRN_INVALID_FORMAT;
      }
      ExpressionNode *arg1 = stack_[stack_.size() - 2];
      ExpressionNode *arg2 = stack_[stack_.size() - 1];
      rc = create_binary_node(operator_type, arg1, arg2, &node);
      if (rc == GRN_SUCCESS) {
        stack_.resize(stack_.size() - 2);
      }
      break;
    }
    default: {
      return GRN_INVALID_ARGUMENT;
    }
  }
  if (rc == GRN_SUCCESS) {
    stack_.push_back(node);
    update_types();
  }
  return rc;
}

grn_rc Expression::filter(
  Record *input, size_t input_size,
  Record *output, size_t *output_size) {
  if ((!input && (input_size != 0)) ||
      ((output > input) && (output < (input + input_size))) || !output_size) {
    return GRN_INVALID_ARGUMENT;
  }
  ExpressionNode *root = this->root();
  if (!root) {
    return GRN_UNKNOWN_ERROR;
  }
  if (!output) {
    output = input;
  }
  size_t total_output_size = 0;
  while (input_size > 0) {
    size_t batch_input_size = GRN_EGN_MAX_BATCH_SIZE;
    if (input_size < batch_input_size) {
      batch_input_size = input_size;
    }
    size_t batch_output_size;
    grn_rc rc = root->filter(
      input, batch_input_size, output, &batch_output_size);
    if (rc != GRN_SUCCESS) {
      return rc;
    }
    input += batch_input_size;
    input_size -= batch_input_size;
    output += batch_output_size;
    total_output_size += batch_output_size;
  }
  *output_size = total_output_size;
  return GRN_SUCCESS;
}

grn_rc Expression::adjust(Record *records, size_t num_records) {
  if (!records && (num_records != 0)) {
    return GRN_INVALID_ARGUMENT;
  }
  ExpressionNode *root = this->root();
  if (!root) {
    return GRN_UNKNOWN_ERROR;
  }
  while (num_records > 0) {
    size_t batch_size = GRN_EGN_MAX_BATCH_SIZE;
    if (num_records < batch_size) {
      batch_size = num_records;
    }
    grn_rc rc = root->adjust(records, batch_size);
    if (rc != GRN_SUCCESS) {
      return rc;
    }
    records += batch_size;
    num_records -= batch_size;
  }
  return GRN_SUCCESS;
}

template <typename T>
grn_rc Expression::evaluate(
  const Record *records, size_t num_records, T *results) {
  if (((!records || !results) && (num_records != 0)) ||
      (T::data_type() != data_type())) {
    return GRN_INVALID_ARGUMENT;
  }
  ExpressionNode *root = this->root();
  if (!root) {
    return GRN_UNKNOWN_ERROR;
  }
  // FIXME: Records should be processed per block.
  //        However, the contents of old blocks will be lost.
  return static_cast<TypedNode<T> *>(root)->evaluate(
    records, num_records, results);
}

template grn_rc Expression::evaluate(
  const Record *records, size_t num_records, Bool *results);
template grn_rc Expression::evaluate(
  const Record *records, size_t num_records, Int *results);
template grn_rc Expression::evaluate(
  const Record *records, size_t num_records, Float *results);
template grn_rc Expression::evaluate(
  const Record *records, size_t num_records, Time *results);
template grn_rc Expression::evaluate(
  const Record *records, size_t num_records, Text *results);
template grn_rc Expression::evaluate(
  const Record *records, size_t num_records, GeoPoint *results);

ExpressionNode *Expression::root() const {
  if (stack_.size() != 1) {
    return NULL;
  }
  return stack_.front();
}

void Expression::update_types() {
  ExpressionNode *root = this->root();
  if (!root) {
    type_ = GRN_EGN_INCOMPLETE;
    data_type_ = GRN_DB_VOID;
  } else {
    switch (root->type()) {
      case GRN_EGN_ID_NODE: {
        type_ = GRN_EGN_ID;
        break;
      }
      case GRN_EGN_SCORE_NODE: {
        type_ = GRN_EGN_SCORE;
        break;
      }
      case GRN_EGN_CONSTANT_NODE: {
        type_ = GRN_EGN_CONSTANT;
        break;
      }
      case GRN_EGN_COLUMN_NODE:
      case GRN_EGN_OPERATOR_NODE: {
        type_ = GRN_EGN_VARIABLE;
        break;
      }
      default: {
        type_ = GRN_EGN_INCOMPLETE;
        break;
      }
    }
    data_type_ = root->data_type();
  }
}

grn_rc Expression::push_bulk_object(grn_obj *obj) {
  grn_rc rc;
  ExpressionNode *node;
  switch (obj->header.domain) {
    case GRN_DB_BOOL: {
      rc = ConstantNode<Bool>::open(Bool(GRN_BOOL_VALUE(obj)), &node);
      break;
    }
    case GRN_DB_INT8: {
      rc = ConstantNode<Int>::open(Int(GRN_INT8_VALUE(obj)), &node);
      break;
    }
    case GRN_DB_INT16: {
      rc = ConstantNode<Int>::open(Int(GRN_INT16_VALUE(obj)), &node);
      break;
    }
    case GRN_DB_INT32: {
      rc = ConstantNode<Int>::open(Int(GRN_INT32_VALUE(obj)), &node);
      break;
    }
    case GRN_DB_INT64: {
      rc = ConstantNode<Int>::open(Int(GRN_INT64_VALUE(obj)), &node);
      break;
    }
    case GRN_DB_UINT8: {
      rc = ConstantNode<Int>::open(Int(GRN_UINT8_VALUE(obj)), &node);
      break;
    }
    case GRN_DB_UINT16: {
      rc = ConstantNode<Int>::open(Int(GRN_UINT16_VALUE(obj)), &node);
      break;
    }
    case GRN_DB_UINT32: {
      rc = ConstantNode<Int>::open(Int(GRN_UINT32_VALUE(obj)), &node);
      break;
    }
    case GRN_DB_UINT64: {
      // FIXME: Type conversion from UInt64 to Int may lose the content.
      rc = ConstantNode<Int>::open(Int(GRN_UINT64_VALUE(obj)), &node);
      break;
    }
    case GRN_DB_FLOAT: {
      rc = ConstantNode<Float>::open(Float(GRN_FLOAT_VALUE(obj)), &node);
      break;
    }
    case GRN_DB_TIME: {
      rc = ConstantNode<Time>::open(Time(GRN_TIME_VALUE(obj)), &node);
      break;
    }
    case GRN_DB_SHORT_TEXT:
    case GRN_DB_TEXT:
    case GRN_DB_LONG_TEXT: {
      Text value(GRN_TEXT_VALUE(obj), GRN_TEXT_LEN(obj));
      rc = ConstantNode<Text>::open(value, &node);
      break;
    }
    // TODO: TokyoGeoPoint and Wgs84GeoPoint should be provided?
    case GRN_DB_TOKYO_GEO_POINT:
    case GRN_DB_WGS84_GEO_POINT: {
      GeoPoint value;
      GRN_GEO_POINT_VALUE(obj, value.raw.latitude, value.raw.longitude);
      rc = ConstantNode<GeoPoint>::open(value, &node);
      break;
    }
    default: {
      return GRN_INVALID_ARGUMENT;
    }
  }
  if (rc == GRN_SUCCESS) try {
    stack_.push_back(node);
  } catch (const std::bad_alloc &) {
    delete node;
    return GRN_NO_MEMORY_AVAILABLE;
  }
  return rc;
}

grn_rc Expression::push_column_object(grn_obj *obj) {
  grn_obj *owner_table = grn_column_table(ctx_, obj);
  if (owner_table != table_) {
    return GRN_INVALID_ARGUMENT;
  }
  grn_id range = grn_obj_get_range(ctx_, obj);
  grn_rc rc;
  ExpressionNode *node;
  switch (obj->header.type) {
    case GRN_COLUMN_FIX_SIZE: {
      switch (range) {
        case GRN_DB_BOOL: {
          rc = ColumnNode<Bool>::open(ctx_, obj, &node);
          break;
        }
        case GRN_DB_INT8:
        case GRN_DB_INT16:
        case GRN_DB_INT32:
        case GRN_DB_INT64:
        case GRN_DB_UINT8:
        case GRN_DB_UINT16:
        case GRN_DB_UINT32:
        case GRN_DB_UINT64: {
          rc = ColumnNode<Int>::open(ctx_, obj, &node);
          break;
        }
        case GRN_DB_FLOAT: {
          rc = ColumnNode<Float>::open(ctx_, obj, &node);
          break;
        }
        case GRN_DB_TIME: {
          rc = ColumnNode<Time>::open(ctx_, obj, &node);
          break;
        }
        case GRN_DB_TOKYO_GEO_POINT:
        case GRN_DB_WGS84_GEO_POINT: {
          rc = ColumnNode<GeoPoint>::open(ctx_, obj, &node);
          break;
        }
        default: {
          return GRN_INVALID_ARGUMENT;
        }
      }
      break;
    }
    case GRN_COLUMN_VAR_SIZE: {
      grn_obj_flags column_type = obj->header.flags & GRN_OBJ_COLUMN_TYPE_MASK;
      switch (column_type) {
        case GRN_OBJ_COLUMN_SCALAR: {
          switch (range) {
            case GRN_DB_SHORT_TEXT:
            case GRN_DB_TEXT:
            case GRN_DB_LONG_TEXT: {
              rc = ColumnNode<Text>::open(ctx_, obj, &node);
              break;
            }
            default: {
              return GRN_INVALID_ARGUMENT;
            }
            break;
          }
          break;
        }
        case GRN_OBJ_COLUMN_VECTOR: {
          return GRN_OPERATION_NOT_SUPPORTED;
        }
        default: {
          return GRN_INVALID_ARGUMENT;
        }
      }
      break;
    }
    default: {
      return GRN_INVALID_ARGUMENT;
    }
  }
  if (rc == GRN_SUCCESS) try {
    stack_.push_back(node);
  } catch (const std::bad_alloc &) {
    delete node;
    return GRN_NO_MEMORY_AVAILABLE;
  }
  return rc;
}

grn_rc Expression::create_unary_node(OperatorType operator_type,
  ExpressionNode *arg, ExpressionNode **node) {
  grn_rc rc = GRN_SUCCESS;
  switch (operator_type) {
    case GRN_OP_NOT: {
      if (arg->data_type() != GRN_DB_BOOL) {
        return GRN_UNKNOWN_ERROR;
      }
      rc = LogicalNotNode::open(arg, node);
      break;
    }
    default: {
      return GRN_INVALID_ARGUMENT;
    }
  }
  return rc;
}

grn_rc Expression::create_binary_node(OperatorType operator_type,
  ExpressionNode *arg1, ExpressionNode *arg2, ExpressionNode **node) {
  switch (operator_type) {
    case GRN_OP_AND: {
      if ((arg1->data_type() != GRN_DB_BOOL) ||
          (arg1->data_type() != GRN_DB_BOOL)) {
        return GRN_INVALID_FORMAT;
      }
      return LogicalAndNode::open(arg1, arg2, node);
    }
    case GRN_OP_OR: {
      if ((arg1->data_type() != GRN_DB_BOOL) ||
          (arg1->data_type() != GRN_DB_BOOL)) {
        return GRN_INVALID_FORMAT;
      }
      return LogicalOrNode::open(arg1, arg2, node);
    }
    case GRN_OP_EQUAL: {
      if (arg1->data_type() != arg2->data_type()) {
        return GRN_INVALID_FORMAT;
      }
      switch (arg1->data_type()) {
        case GRN_DB_BOOL: {
          return equal_node_open(EqualOperator<Bool>(), arg1, arg2, node);
        }
        case GRN_DB_INT64: {
          return equal_node_open(EqualOperator<Int>(), arg1, arg2, node);
        }
        case GRN_DB_FLOAT: {
          return equal_node_open(EqualOperator<Float>(), arg1, arg2, node);
        }
        case GRN_DB_TIME: {
          return equal_node_open(EqualOperator<Time>(), arg1, arg2, node);
        }
        case GRN_DB_TEXT: {
          return equal_node_open(EqualOperator<Text>(), arg1, arg2, node);
        }
        case GRN_DB_WGS84_GEO_POINT: {
          return equal_node_open(EqualOperator<GeoPoint>(), arg1, arg2, node);
        }
        default: {
          return GRN_UNKNOWN_ERROR;
        }
      }
    }
    case GRN_OP_NOT_EQUAL: {
      if (arg1->data_type() != arg2->data_type()) {
        return GRN_INVALID_FORMAT;
      }
      switch (arg1->data_type()) {
        case GRN_DB_BOOL: {
          return not_equal_node_open(
            NotEqualOperator<Bool>(), arg1, arg2, node);
        }
        case GRN_DB_INT64: {
          return not_equal_node_open(
            NotEqualOperator<Int>(), arg1, arg2, node);
        }
        case GRN_DB_FLOAT: {
          return not_equal_node_open(
            NotEqualOperator<Float>(), arg1, arg2, node);
        }
        case GRN_DB_TIME: {
          return not_equal_node_open(
            NotEqualOperator<Time>(), arg1, arg2, node);
        }
        case GRN_DB_TEXT: {
          return not_equal_node_open(
            NotEqualOperator<Text>(), arg1, arg2, node);
        }
        case GRN_DB_WGS84_GEO_POINT: {
          return not_equal_node_open(
            NotEqualOperator<GeoPoint>(), arg1, arg2, node);
        }
        default: {
          return GRN_UNKNOWN_ERROR;
        }
      }
    }
    case GRN_OP_LESS: {
      if (arg1->data_type() != arg2->data_type()) {
        return GRN_INVALID_FORMAT;
      }
      switch (arg1->data_type()) {
        case GRN_DB_INT64: {
          return less_node_open(LessOperator<Int>(), arg1, arg2, node);
        }
        case GRN_DB_FLOAT: {
          return less_node_open(LessOperator<Float>(), arg1, arg2, node);
        }
        case GRN_DB_TIME: {
          return less_node_open(LessOperator<Time>(), arg1, arg2, node);
        }
        case GRN_DB_TEXT: {
          return less_node_open(LessOperator<Text>(), arg1, arg2, node);
        }
        default: {
          return GRN_UNKNOWN_ERROR;
        }
      }
    }
    case GRN_OP_LESS_EQUAL: {
      if (arg1->data_type() != arg2->data_type()) {
        return GRN_INVALID_FORMAT;
      }
      switch (arg1->data_type()) {
        case GRN_DB_INT64: {
          return less_equal_node_open(
            LessEqualOperator<Int>(), arg1, arg2, node);
        }
        case GRN_DB_FLOAT: {
          return less_equal_node_open(
            LessEqualOperator<Float>(), arg1, arg2, node);
        }
        case GRN_DB_TIME: {
          return less_equal_node_open(
            LessEqualOperator<Time>(), arg1, arg2, node);
        }
        case GRN_DB_TEXT: {
          return less_equal_node_open(
            LessEqualOperator<Text>(), arg1, arg2, node);
        }
        default: {
          return GRN_UNKNOWN_ERROR;
        }
      }
    }
    case GRN_OP_GREATER: {
      if (arg1->data_type() != arg2->data_type()) {
        return GRN_INVALID_FORMAT;
      }
      switch (arg1->data_type()) {
        case GRN_DB_INT64: {
          return greater_node_open(GreaterOperator<Int>(), arg1, arg2, node);
        }
        case GRN_DB_FLOAT: {
          return greater_node_open(GreaterOperator<Float>(), arg1, arg2, node);
        }
        case GRN_DB_TIME: {
          return greater_node_open(GreaterOperator<Time>(), arg1, arg2, node);
        }
        case GRN_DB_TEXT: {
          return greater_node_open(GreaterOperator<Text>(), arg1, arg2, node);
        }
        default: {
          return GRN_UNKNOWN_ERROR;
        }
      }
    }
    case GRN_OP_GREATER_EQUAL: {
      if (arg1->data_type() != arg2->data_type()) {
        return GRN_INVALID_FORMAT;
      }
      switch (arg1->data_type()) {
        case GRN_DB_INT64: {
          return greater_equal_node_open(
            GreaterEqualOperator<Int>(), arg1, arg2, node);
        }
        case GRN_DB_FLOAT: {
          return greater_equal_node_open(
            GreaterEqualOperator<Float>(), arg1, arg2, node);
        }
        case GRN_DB_TIME: {
          return greater_equal_node_open(
            GreaterEqualOperator<Time>(), arg1, arg2, node);
        }
        case GRN_DB_TEXT: {
          return greater_equal_node_open(
            GreaterEqualOperator<Text>(), arg1, arg2, node);
        }
        default: {
          return GRN_UNKNOWN_ERROR;
        }
      }
    }
    default: {
      return GRN_INVALID_ARGUMENT;
    }
  }
}

}  // namespace egn
}  // namespace grn

#ifdef __cplusplus
extern "C" {
#endif

static grn_rc
grn_egn_select_filter(grn_ctx *ctx, grn_obj *table,
                      const char *filter, size_t filter_size,
                      int offset, int limit,
                      std::vector<grn_egn_record> *records,
                      size_t *num_hits) {
  if (offset < 0) {
    offset = 0;
  }
  if (limit < 0) {
    limit = std::numeric_limits<int>::max();
  }
  grn::egn::Cursor *cursor;
  grn_rc rc = grn::egn::Cursor::open_table_cursor(ctx, table, &cursor);
  if (rc == GRN_SUCCESS) {
    grn::egn::Expression *expression;
    rc = grn::egn::Expression::parse(
      ctx, table, filter, filter_size, &expression);
    if (rc == GRN_SUCCESS) {
      size_t count = 0;
      for ( ; ; ) {
        size_t records_offset = records->size();
        try {
          records->resize(records->size() + GRN_EGN_MAX_BATCH_SIZE);
        } catch (const std::bad_alloc &) {
          rc = GRN_NO_MEMORY_AVAILABLE;
          break;
        }
        size_t batch_size;
        rc = cursor->read(&(*records)[records_offset],
                          GRN_EGN_MAX_BATCH_SIZE, &batch_size);
        if (rc != GRN_SUCCESS) {
          break;
        }
        if (batch_size == 0) {
          records->resize(records_offset);
          break;
        }
        rc = expression->filter(&(*records)[records_offset], batch_size,
                                NULL, &batch_size);
        if (rc != GRN_SUCCESS) {
          break;
        }
        count += batch_size;
        if (offset > 0) {
          if (offset >= batch_size) {
            offset -= batch_size;
            batch_size = 0;
          } else {
            std::memcpy(&(*records)[0], &(*records)[offset],
                        sizeof(grn_egn_record) * (batch_size - offset));
            batch_size -= offset;
            offset = 0;
          }
        }
        if (limit >= batch_size) {
          limit -= batch_size;
        } else {
          batch_size = limit;
          limit = 0;
        }
        records->resize(records_offset + batch_size);
      }
      delete expression;
      *num_hits = count;
    }
    delete cursor;
  }
  return rc;
}

static grn_rc
grn_egn_select_output(grn_ctx *ctx, grn_obj *table,
                      const char *output_columns, size_t output_columns_size,
                      const grn_egn_record *records, size_t num_records,
                      size_t num_hits) {
  grn_rc rc = GRN_SUCCESS;
  std::vector<std::string> names;
  std::vector<grn::egn::Expression *> expressions;

  const char *rest = output_columns;
  size_t rest_size = output_columns_size;
  while (rest_size != 0) {
    size_t pos;
    for (pos = 0; pos < rest_size; ++pos) {
      if ((rest[pos] != ',') &&
          !std::isspace(static_cast<unsigned char>(rest[pos]))) {
        break;
      }
    }
    if (pos >= rest_size) {
      break;
    }
    rest += pos;
    rest_size -= pos;
    for (pos = 0; pos < rest_size; ++pos) {
      if ((rest[pos] == ',') ||
          std::isspace(static_cast<unsigned char>(rest[pos]))) {
        break;
      }
    }
    // TODO: Error handling.
    std::string name(rest, pos);
    if (name == "*") {
      grn_hash *columns;
      if ((columns = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                                     GRN_OBJ_TABLE_HASH_KEY|GRN_HASH_TINY))) {
        if (grn_table_columns(ctx, table, "", 0, (grn_obj *)columns)) {
          grn_id *key;
          GRN_HASH_EACH(ctx, columns, id, &key, NULL, NULL, {
            grn_obj *column = grn_ctx_at(ctx, *key);
            if (column) {
              char name_buf[1024];
              int name_size = grn_column_name(
                ctx, column, name_buf, sizeof(name_buf));
              grn::egn::Expression *expression;
              grn_rc r = grn::egn::Expression::open(ctx, table, &expression);
              if (r == GRN_SUCCESS) {
                r = expression->push_object(column);
                if (r == GRN_SUCCESS) {
                  names.push_back(std::string(name_buf, name_size));
                  expressions.push_back(expression);
                }
              }
            }
          });
        }
        grn_hash_close(ctx, columns);
      }
    } else {
      grn::egn::Expression *expression;
      grn_rc r = grn::egn::Expression::parse(
        ctx, table, rest, pos, &expression);
      if (r == GRN_SUCCESS) {
        names.push_back(name);
        expressions.push_back(expression);
      }
    }
    if (pos >= rest_size) {
      break;
    }
    rest += pos + 1;
    rest_size -= pos + 1;
  }

  GRN_OUTPUT_ARRAY_OPEN("RESULT", 1);
  GRN_OUTPUT_ARRAY_OPEN("RESULTSET", 2 + num_records);
  GRN_OUTPUT_ARRAY_OPEN("NHITS", 1);
  grn_text_ulltoa(ctx, ctx->impl->outbuf, num_hits);
  GRN_OUTPUT_ARRAY_CLOSE();  // NHITS.
  GRN_OUTPUT_ARRAY_OPEN("COLUMNS", expressions.size());
  for (size_t i = 0; i < expressions.size(); ++i) {
    GRN_OUTPUT_ARRAY_OPEN("COLUMN", 2);
    GRN_TEXT_PUTC(ctx, ctx->impl->outbuf, '"');
    GRN_TEXT_PUT(ctx, ctx->impl->outbuf, names[i].data(), names[i].size());
    GRN_TEXT_PUT(ctx, ctx->impl->outbuf, "\",\"", 3);
    switch (expressions[i]->data_type()) {
      case GRN_DB_BOOL: {
        GRN_TEXT_PUTS(ctx, ctx->impl->outbuf, "Bool");
        break;
      }
      case GRN_DB_INT64: {
        GRN_TEXT_PUTS(ctx, ctx->impl->outbuf, "Int64");
        break;
      }
      case GRN_DB_FLOAT: {
        GRN_TEXT_PUTS(ctx, ctx->impl->outbuf, "Float");
        break;
      }
      case GRN_DB_TIME: {
        GRN_TEXT_PUTS(ctx, ctx->impl->outbuf, "Time");
        break;
      }
      case GRN_DB_SHORT_TEXT:
      case GRN_DB_TEXT:
      case GRN_DB_LONG_TEXT: {
        GRN_TEXT_PUTS(ctx, ctx->impl->outbuf, "Text");
        break;
      }
      case GRN_DB_WGS84_GEO_POINT: {
        GRN_TEXT_PUTS(ctx, ctx->impl->outbuf, "GeoPoint");
        break;
      }
      default: {
        GRN_TEXT_PUTS(ctx, ctx->impl->outbuf, "N/A");
        break;
      }
    }
    GRN_TEXT_PUTC(ctx, ctx->impl->outbuf, '"');
    GRN_OUTPUT_ARRAY_CLOSE();
  }
  GRN_OUTPUT_ARRAY_CLOSE();  // COLUMNS.
  if (num_records != 0) {
    size_t count = 0;
    std::vector<std::vector<char> > bufs(expressions.size());
    while (count < num_records) {
      size_t batch_size = GRN_EGN_MAX_BATCH_SIZE;
      if (batch_size > (num_records - count)) {
        batch_size = num_records - count;
      }
      for (size_t i = 0; i < expressions.size(); ++i) {
        switch (expressions[i]->data_type()) {
          case GRN_DB_BOOL: {
            bufs[i].resize(sizeof(grn_egn_bool) * batch_size);
            expressions[i]->evaluate(records + count, batch_size,
                                     (grn::egn::Bool *)&bufs[i][0]);
            break;
          }
          case GRN_DB_INT64: {
            bufs[i].resize(sizeof(grn_egn_int) * batch_size);
            expressions[i]->evaluate(records + count, batch_size,
                                     (grn::egn::Int *)&bufs[i][0]);
            break;
          }
          case GRN_DB_FLOAT: {
            bufs[i].resize(sizeof(grn_egn_float) * batch_size);
            expressions[i]->evaluate(records + count, batch_size,
                                     (grn::egn::Float *)&bufs[i][0]);
            break;
          }
          case GRN_DB_TIME: {
            bufs[i].resize(sizeof(grn_egn_time) * batch_size);
            expressions[i]->evaluate(records + count, batch_size,
                                     (grn::egn::Time *)&bufs[i][0]);
            break;
          }
          case GRN_DB_TEXT: {
            bufs[i].resize(sizeof(grn_egn_text) * batch_size);
            expressions[i]->evaluate(records + count, batch_size,
                                     (grn::egn::Text *)&bufs[i][0]);
            break;
          }
          case GRN_DB_WGS84_GEO_POINT: {
            bufs[i].resize(sizeof(grn_egn_geo_point) * batch_size);
            expressions[i]->evaluate(records + count, batch_size,
                                     (grn::egn::GeoPoint *)&bufs[i][0]);
            break;
          }
          default: {
            break;
          }
        }
      }
      for (size_t i = 0; i < batch_size; ++i) {
        GRN_OUTPUT_ARRAY_OPEN("HIT", expressions.size());
        for (size_t j = 0; j < expressions.size(); ++j) {
          if (j != 0) {
            GRN_TEXT_PUTC(ctx, ctx->impl->outbuf, ',');
          }
          switch (expressions[j]->data_type()) {
            case GRN_DB_BOOL: {
              if (((grn_egn_bool *)&bufs[j][0])[i]) {
                GRN_TEXT_PUT(ctx, ctx->impl->outbuf, "true", 4);
              } else {
                GRN_TEXT_PUT(ctx, ctx->impl->outbuf, "false", 5);
              }
              break;
            }
            case GRN_DB_INT64: {
              grn_text_lltoa(ctx, ctx->impl->outbuf,
                             ((grn_egn_int *)&bufs[j][0])[i]);
              break;
            }
            case GRN_DB_FLOAT: {
              grn_text_ftoa(ctx, ctx->impl->outbuf,
                            ((grn_egn_float *)&bufs[j][0])[i]);
              break;
            }
            case GRN_DB_TIME: {
              grn_text_ftoa(ctx, ctx->impl->outbuf,
                            ((grn_egn_time *)&bufs[j][0])[i] * 0.000001);
              break;
            }
            case GRN_DB_TEXT: {
              grn_egn_text text = ((grn_egn_text *)&bufs[j][0])[i];
              grn_text_esc(ctx, ctx->impl->outbuf, text.ptr, text.size);
              break;
            }
            case GRN_DB_WGS84_GEO_POINT: {
              grn_egn_geo_point geo_point =
                ((grn_egn_geo_point *)&bufs[j][0])[i];
              GRN_TEXT_PUTC(ctx, ctx->impl->outbuf, '"');
              grn_text_itoa(ctx, ctx->impl->outbuf, geo_point.latitude);
              GRN_TEXT_PUTC(ctx, ctx->impl->outbuf, 'x');
              grn_text_itoa(ctx, ctx->impl->outbuf, geo_point.longitude);
              GRN_TEXT_PUTC(ctx, ctx->impl->outbuf, '"');
              break;
            }
            default: {
              break;
            }
          }
        }
        GRN_OUTPUT_ARRAY_CLOSE();  // HITS.
      }
      count += batch_size;
    }
  }
  GRN_OUTPUT_ARRAY_CLOSE();  // RESULTSET.
  GRN_OUTPUT_ARRAY_CLOSE();  // RESET.
  for (size_t i = 0; i < expressions.size(); ++i) {
    delete expressions[i];
  }
  return rc;
}

grn_rc
grn_egn_select(grn_ctx *ctx, grn_obj *table,
               const char *filter, size_t filter_size,
               const char *output_columns, size_t output_columns_size,
               int offset, int limit) {
  if (!ctx || !grn_egn_is_table(table) || (!filter && (filter_size != 0)) ||
      (!output_columns && (output_columns_size != 0))) {
    return GRN_INVALID_ARGUMENT;
  }
  std::vector<grn_egn_record> records;
  size_t num_hits;
  grn_rc rc = grn_egn_select_filter(ctx, table, filter, filter_size,
                                    offset, limit, &records, &num_hits);
  if (rc == GRN_SUCCESS) {
    rc = grn_egn_select_output(ctx, table, output_columns, output_columns_size,
                               &*records.begin(), records.size(), num_hits);
  }
  return rc;
}

#ifdef __cplusplus
}
#endif

#endif  // GRN_WITH_EGN
