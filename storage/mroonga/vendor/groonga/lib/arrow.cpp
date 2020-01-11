/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2017 Brazil

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

#include "grn.h"
#include "grn_db.h"

#ifdef GRN_WITH_ARROW
#include <groonga/arrow.hpp>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <arrow/ipc/api.h>

#include <sstream>

namespace grnarrow {
  grn_rc status_to_rc(arrow::Status &status) {
    switch (status.code()) {
    case arrow::StatusCode::OK:
      return GRN_SUCCESS;
    case arrow::StatusCode::OutOfMemory:
      return GRN_NO_MEMORY_AVAILABLE;
    case arrow::StatusCode::KeyError:
      return GRN_INVALID_ARGUMENT; // TODO
    case arrow::StatusCode::TypeError:
      return GRN_INVALID_ARGUMENT; // TODO
    case arrow::StatusCode::Invalid:
      return GRN_INVALID_ARGUMENT;
    case arrow::StatusCode::IOError:
      return GRN_INPUT_OUTPUT_ERROR;
    case arrow::StatusCode::UnknownError:
      return GRN_UNKNOWN_ERROR;
    case arrow::StatusCode::NotImplemented:
      return GRN_FUNCTION_NOT_IMPLEMENTED;
    default:
      return GRN_UNKNOWN_ERROR;
    }
  }

  grn_bool check_status(grn_ctx *ctx,
                        arrow::Status &status,
                        const char *context) {
    if (status.ok()) {
      return GRN_TRUE;
    } else {
      auto rc = status_to_rc(status);
      auto message = status.ToString();
      ERR(rc, "%s: %s", context, message.c_str());
      return GRN_FALSE;
    }
  }

  grn_bool check_status(grn_ctx *ctx,
                        arrow::Status &status,
                        std::ostream &output) {
    return check_status(ctx,
                        status,
                        static_cast<std::stringstream &>(output).str().c_str());
  }

  class ColumnLoadVisitor : public arrow::ArrayVisitor {
  public:
    ColumnLoadVisitor(grn_ctx *ctx,
                      grn_obj *grn_table,
                      std::shared_ptr<arrow::Column> &arrow_column,
                      const grn_id *ids)
      : ctx_(ctx),
        grn_table_(grn_table),
        ids_(ids),
        time_unit_(arrow::TimeUnit::SECOND) {
      auto column_name = arrow_column->name();
      grn_column_ = grn_obj_column(ctx_, grn_table_,
                                   column_name.data(),
                                   column_name.size());

      auto arrow_type = arrow_column->type();
      grn_id type_id;
      switch (arrow_type->id()) {
      case arrow::Type::BOOL :
        type_id = GRN_DB_BOOL;
        break;
      case arrow::Type::UINT8 :
        type_id = GRN_DB_UINT8;
        break;
      case arrow::Type::INT8 :
        type_id = GRN_DB_INT8;
        break;
      case arrow::Type::UINT16 :
        type_id = GRN_DB_UINT16;
        break;
      case arrow::Type::INT16 :
        type_id = GRN_DB_INT16;
        break;
      case arrow::Type::UINT32 :
        type_id = GRN_DB_UINT32;
        break;
      case arrow::Type::INT32 :
        type_id = GRN_DB_INT32;
        break;
      case arrow::Type::UINT64 :
        type_id = GRN_DB_UINT64;
        break;
      case arrow::Type::INT64 :
        type_id = GRN_DB_INT64;
        break;
      case arrow::Type::HALF_FLOAT :
      case arrow::Type::FLOAT :
      case arrow::Type::DOUBLE :
        type_id = GRN_DB_FLOAT;
        break;
      case arrow::Type::STRING :
        type_id = GRN_DB_TEXT;
        break;
      case arrow::Type::DATE64 :
        type_id = GRN_DB_TIME;
        break;
      case arrow::Type::TIMESTAMP :
        type_id = GRN_DB_TIME;
        {
          auto arrow_timestamp_type =
            std::static_pointer_cast<arrow::TimestampType>(arrow_type);
          time_unit_ = arrow_timestamp_type->unit();
        }
        break;
      default :
        type_id = GRN_DB_VOID;
        break;
      }

      if (type_id == GRN_DB_VOID) {
        // TODO
        return;
      }

      if (!grn_column_) {
        grn_column_ = grn_column_create(ctx_,
                                        grn_table_,
                                        column_name.data(),
                                        column_name.size(),
                                        NULL,
                                        GRN_OBJ_COLUMN_SCALAR,
                                        grn_ctx_at(ctx_, type_id));
      }
      if (type_id == GRN_DB_TEXT) {
        GRN_TEXT_INIT(&buffer_, GRN_OBJ_DO_SHALLOW_COPY);
      } else {
        GRN_VALUE_FIX_SIZE_INIT(&buffer_, 0, type_id);
      }
    }

    ~ColumnLoadVisitor() {
      if (grn_obj_is_accessor(ctx_, grn_column_)) {
        grn_obj_unlink(ctx_, grn_column_);
      }
      GRN_OBJ_FIN(ctx_, &buffer_);
    }

    arrow::Status Visit(const arrow::BooleanArray &array) {
      return set_values(array);
    }

    arrow::Status Visit(const arrow::Int8Array &array) {
      return set_values(array);
    }

    arrow::Status Visit(const arrow::UInt8Array &array) {
      return set_values(array);
    }

    arrow::Status Visit(const arrow::Int16Array &array) {
      return set_values(array);
    }

    arrow::Status Visit(const arrow::UInt16Array &array) {
      return set_values(array);
    }

    arrow::Status Visit(const arrow::Int32Array &array) {
      return set_values(array);
    }

    arrow::Status Visit(const arrow::UInt32Array &array) {
      return set_values(array);
    }

    arrow::Status Visit(const arrow::Int64Array &array) {
      return set_values(array);
    }

    arrow::Status Visit(const arrow::UInt64Array &array) {
      return set_values(array);
    }

    arrow::Status Visit(const arrow::HalfFloatArray &array) {
      return set_values(array);
    }

    arrow::Status Visit(const arrow::FloatArray &array) {
      return set_values(array);
    }

    arrow::Status Visit(const arrow::DoubleArray &array) {
      return set_values(array);
    }

    arrow::Status Visit(const arrow::StringArray &array) {
      return set_values(array);
    }

    arrow::Status Visit(const arrow::Date64Array &array) {
      return set_values(array);
    }

    arrow::Status Visit(const arrow::TimestampArray &array) {
      return set_values(array);
    }

  private:
    grn_ctx *ctx_;
    grn_obj *grn_table_;
    const grn_id *ids_;
    arrow::TimeUnit::type time_unit_;
    grn_obj *grn_column_;
    grn_obj buffer_;

    template <typename T>
    arrow::Status set_values(const T &array) {
      int64_t n_rows = array.length();
      for (int i = 0; i < n_rows; ++i) {
        auto id = ids_[i];
        GRN_BULK_REWIND(&buffer_);
        get_value(array, i);
        grn_obj_set_value(ctx_, grn_column_, id, &buffer_, GRN_OBJ_SET);
      }
      return arrow::Status::OK();
    }

    void
    get_value(const arrow::BooleanArray &array, int i) {
      GRN_BOOL_SET(ctx_, &buffer_, array.Value(i));
    }

    void
    get_value(const arrow::UInt8Array &array, int i) {
      GRN_UINT8_SET(ctx_, &buffer_, array.Value(i));
    }

    void
    get_value(const arrow::Int8Array &array, int i) {
      GRN_INT8_SET(ctx_, &buffer_, array.Value(i));
    }

    void
    get_value(const arrow::UInt16Array &array, int i) {
      GRN_UINT16_SET(ctx_, &buffer_, array.Value(i));
    }

    void
    get_value(const arrow::Int16Array &array, int i) {
      GRN_INT16_SET(ctx_, &buffer_, array.Value(i));
    }

    void
    get_value(const arrow::UInt32Array &array, int i) {
      GRN_UINT32_SET(ctx_, &buffer_, array.Value(i));
    }

    void
    get_value(const arrow::Int32Array &array, int i) {
      GRN_INT32_SET(ctx_, &buffer_, array.Value(i));
    }

    void
    get_value(const arrow::UInt64Array &array, int i) {
      GRN_UINT64_SET(ctx_, &buffer_, array.Value(i));
    }

    void
    get_value(const arrow::Int64Array &array, int i) {
      GRN_INT64_SET(ctx_, &buffer_, array.Value(i));
    }

    void
    get_value(const arrow::HalfFloatArray &array, int i) {
      GRN_FLOAT_SET(ctx_, &buffer_, array.Value(i));
    }

    void
    get_value(const arrow::FloatArray &array, int i) {
      GRN_FLOAT_SET(ctx_, &buffer_, array.Value(i));
    }

    void
    get_value(const arrow::DoubleArray &array, int i) {
      GRN_FLOAT_SET(ctx_, &buffer_, array.Value(i));
    }

    void
    get_value(const arrow::StringArray &array, int i) {
      int32_t size;
      const auto data = array.GetValue(i, &size);
      GRN_TEXT_SET(ctx_, &buffer_, data, size);
    }

    void
    get_value(const arrow::Date64Array &array, int i) {
      GRN_TIME_SET(ctx_, &buffer_, array.Value(i));
    }

    void
    get_value(const arrow::TimestampArray &array, int i) {
      switch (time_unit_) {
      case arrow::TimeUnit::SECOND :
        GRN_TIME_SET(ctx_, &buffer_, GRN_TIME_PACK(array.Value(i), 0));
        break;
      case arrow::TimeUnit::MILLI :
        GRN_TIME_SET(ctx_, &buffer_, array.Value(i) * 1000);
        break;
      case arrow::TimeUnit::MICRO :
        GRN_TIME_SET(ctx_, &buffer_, array.Value(i));
        break;
      case arrow::TimeUnit::NANO :
        GRN_TIME_SET(ctx_, &buffer_, array.Value(i) / 1000);
        break;
      }
    }
  };

  class FileLoader {
  public:
    FileLoader(grn_ctx *ctx, grn_obj *grn_table)
      : ctx_(ctx),
        grn_table_(grn_table),
        key_column_name_("") {
    }

    ~FileLoader() {
    }

    grn_rc load_table(const std::shared_ptr<arrow::Table> &arrow_table) {
      int n_columns = arrow_table->num_columns();

      if (key_column_name_.empty()) {
        grn_obj ids;
        GRN_RECORD_INIT(&ids, GRN_OBJ_VECTOR, grn_obj_id(ctx_, grn_table_));
        auto n_records = arrow_table->num_rows();
        for (int64_t i = 0; i < n_records; ++i) {
          auto id = grn_table_add(ctx_, grn_table_, NULL, 0, NULL);
          GRN_RECORD_PUT(ctx_, &ids, id);
        }
        for (int i = 0; i < n_columns; ++i) {
          int64_t offset = 0;
          auto arrow_column = arrow_table->column(i);
          auto arrow_chunked_data = arrow_column->data();
          for (auto arrow_array : arrow_chunked_data->chunks()) {
            grn_id *sub_ids =
              reinterpret_cast<grn_id *>(GRN_BULK_HEAD(&ids)) + offset;
            ColumnLoadVisitor visitor(ctx_,
                                      grn_table_,
                                      arrow_column,
                                      sub_ids);
            arrow_array->Accept(&visitor);
            offset += arrow_array->length();
          }
        }
        GRN_OBJ_FIN(ctx_, &ids);
      } else {
        auto status = arrow::Status::NotImplemented("_key isn't supported yet");
        check_status(ctx_, status, "[arrow][load]");
      }
      return ctx_->rc;
    };

    grn_rc load_record_batch(const std::shared_ptr<arrow::RecordBatch> &arrow_record_batch) {
      std::shared_ptr<arrow::Table> arrow_table;
      std::vector<std::shared_ptr<arrow::RecordBatch>> arrow_record_batches(1);
      arrow_record_batches[0] = arrow_record_batch;
      auto status =
        arrow::Table::FromRecordBatches(arrow_record_batches, &arrow_table);
      if (!check_status(ctx_,
                        status,
                        "[arrow][load] "
                        "failed to convert record batch to table")) {
        return ctx_->rc;
      }
      return load_table(arrow_table);
    };

  private:
    grn_ctx *ctx_;
    grn_obj *grn_table_;
    std::string key_column_name_;
  };

  class FileDumper {
  public:
    FileDumper(grn_ctx *ctx, grn_obj *grn_table, grn_obj *grn_columns)
      : ctx_(ctx),
        grn_table_(grn_table),
        grn_columns_(grn_columns) {
    }

    ~FileDumper() {
    }

    grn_rc dump(arrow::io::OutputStream *output) {
      std::vector<std::shared_ptr<arrow::Field>> fields;
      auto n_columns = GRN_BULK_VSIZE(grn_columns_) / sizeof(grn_obj *);
      for (auto i = 0; i < n_columns; ++i) {
        auto column = GRN_PTR_VALUE_AT(grn_columns_, i);

        char column_name[GRN_TABLE_MAX_KEY_SIZE];
        int column_name_size;
        column_name_size =
          grn_column_name(ctx_, column, column_name, GRN_TABLE_MAX_KEY_SIZE);
        std::string field_name(column_name, column_name_size);
        std::shared_ptr<arrow::DataType> field_type;
        switch (grn_obj_get_range(ctx_, column)) {
        case GRN_DB_BOOL :
          field_type = arrow::boolean();
          break;
        case GRN_DB_UINT8 :
          field_type = arrow::uint8();
          break;
        case GRN_DB_INT8 :
          field_type = arrow::int8();
          break;
        case GRN_DB_UINT16 :
          field_type = arrow::uint16();
          break;
        case GRN_DB_INT16 :
          field_type = arrow::int16();
          break;
        case GRN_DB_UINT32 :
          field_type = arrow::uint32();
          break;
        case GRN_DB_INT32 :
          field_type = arrow::int32();
          break;
        case GRN_DB_UINT64 :
          field_type = arrow::uint64();
          break;
        case GRN_DB_INT64 :
          field_type = arrow::int64();
          break;
        case GRN_DB_FLOAT :
          field_type = arrow::float64();
          break;
        case GRN_DB_TIME :
          field_type =
            std::make_shared<arrow::TimestampType>(arrow::TimeUnit::MICRO);
          break;
        case GRN_DB_SHORT_TEXT :
        case GRN_DB_TEXT :
        case GRN_DB_LONG_TEXT :
          field_type = arrow::utf8();
          break;
        default :
          break;
        }
        if (!field_type) {
          continue;
        }

        auto field = std::make_shared<arrow::Field>(field_name,
                                                    field_type,
                                                    false);
        fields.push_back(field);
      };

      auto schema = std::make_shared<arrow::Schema>(fields);

      std::shared_ptr<arrow::ipc::RecordBatchFileWriter> writer;
      auto status =
        arrow::ipc::RecordBatchFileWriter::Open(output, schema, &writer);
      if (!check_status(ctx_,
                        status,
                        "[arrow][dump] failed to create file format writer")) {
        return ctx_->rc;
      }

      std::vector<grn_id> ids;
      int n_records_per_batch = 1000;
      GRN_TABLE_EACH_BEGIN(ctx_, grn_table_, table_cursor, record_id) {
        ids.push_back(record_id);
        if (ids.size() == n_records_per_batch) {
          write_record_batch(ids, schema, writer);
          ids.clear();
        }
      } GRN_TABLE_EACH_END(ctx_, table_cursor);
      if (!ids.empty()) {
        write_record_batch(ids, schema, writer);
      }
      writer->Close();

      return ctx_->rc;
    }

  private:
    grn_ctx *ctx_;
    grn_obj *grn_table_;
    grn_obj *grn_columns_;

    void write_record_batch(std::vector<grn_id> &ids,
                            std::shared_ptr<arrow::Schema> &schema,
                            std::shared_ptr<arrow::ipc::RecordBatchFileWriter> &writer) {
      std::vector<std::shared_ptr<arrow::Array>> columns;
      auto n_columns = GRN_BULK_VSIZE(grn_columns_) / sizeof(grn_obj *);
      for (auto i = 0; i < n_columns; ++i) {
        auto grn_column = GRN_PTR_VALUE_AT(grn_columns_, i);

        arrow::Status status;
        std::shared_ptr<arrow::Array> column;

        switch (grn_obj_get_range(ctx_, grn_column)) {
        case GRN_DB_BOOL :
          status = build_boolean_array(ids, grn_column, &column);
          break;
        case GRN_DB_UINT8 :
          status = build_uint8_array(ids, grn_column, &column);
          break;
        case GRN_DB_INT8 :
          status = build_int8_array(ids, grn_column, &column);
          break;
        case GRN_DB_UINT16 :
          status = build_uint16_array(ids, grn_column, &column);
          break;
        case GRN_DB_INT16 :
          status = build_int16_array(ids, grn_column, &column);
          break;
        case GRN_DB_UINT32 :
          status = build_uint32_array(ids, grn_column, &column);
          break;
        case GRN_DB_INT32 :
          status = build_int32_array(ids, grn_column, &column);
          break;
        case GRN_DB_UINT64 :
          status = build_uint64_array(ids, grn_column, &column);
          break;
        case GRN_DB_INT64 :
          status = build_int64_array(ids, grn_column, &column);
          break;
        case GRN_DB_FLOAT :
          status = build_double_array(ids, grn_column, &column);
          break;
        case GRN_DB_TIME :
          status = build_timestamp_array(ids, grn_column, &column);
          break;
        case GRN_DB_SHORT_TEXT :
        case GRN_DB_TEXT :
        case GRN_DB_LONG_TEXT :
          status = build_utf8_array(ids, grn_column, &column);
          break;
        default :
          status =
            arrow::Status::NotImplemented("[arrow][dumper] not supported type: TODO");
          break;
        }
        if (!status.ok()) {
          continue;
        }
        columns.push_back(column);
      }

      arrow::RecordBatch record_batch(schema, ids.size(), columns);
      writer->WriteRecordBatch(record_batch);
    }

    arrow::Status build_boolean_array(std::vector<grn_id> &ids,
                                      grn_obj *grn_column,
                                      std::shared_ptr<arrow::Array> *array) {
      arrow::BooleanBuilder builder(arrow::default_memory_pool());
      for (auto id : ids) {
        uint32_t size;
        auto data = grn_obj_get_value_(ctx_, grn_column, id, &size);
        builder.Append(*(reinterpret_cast<const grn_bool *>(data)));
      }
      return builder.Finish(array);
    }

    arrow::Status build_uint8_array(std::vector<grn_id> &ids,
                                    grn_obj *grn_column,
                                    std::shared_ptr<arrow::Array> *array) {
      arrow::UInt8Builder builder(arrow::default_memory_pool());
      for (auto id : ids) {
        uint32_t size;
        auto data = grn_obj_get_value_(ctx_, grn_column, id, &size);
        builder.Append(*(reinterpret_cast<const uint8_t *>(data)));
      }
      return builder.Finish(array);
    }

    arrow::Status build_int8_array(std::vector<grn_id> &ids,
                                   grn_obj *grn_column,
                                   std::shared_ptr<arrow::Array> *array) {
      arrow::Int8Builder builder(arrow::default_memory_pool());
      for (auto id : ids) {
        uint32_t size;
        auto data = grn_obj_get_value_(ctx_, grn_column, id, &size);
        builder.Append(*(reinterpret_cast<const int8_t *>(data)));
      }
      return builder.Finish(array);
    }

    arrow::Status build_uint16_array(std::vector<grn_id> &ids,
                                     grn_obj *grn_column,
                                     std::shared_ptr<arrow::Array> *array) {
      arrow::UInt16Builder builder(arrow::default_memory_pool());
      for (auto id : ids) {
        uint32_t size;
        auto data = grn_obj_get_value_(ctx_, grn_column, id, &size);
        builder.Append(*(reinterpret_cast<const uint16_t *>(data)));
      }
      return builder.Finish(array);
    }

    arrow::Status build_int16_array(std::vector<grn_id> &ids,
                                    grn_obj *grn_column,
                                    std::shared_ptr<arrow::Array> *array) {
      arrow::Int16Builder builder(arrow::default_memory_pool());
      for (auto id : ids) {
        uint32_t size;
        auto data = grn_obj_get_value_(ctx_, grn_column, id, &size);
        builder.Append(*(reinterpret_cast<const int16_t *>(data)));
      }
      return builder.Finish(array);
    }

    arrow::Status build_uint32_array(std::vector<grn_id> &ids,
                                     grn_obj *grn_column,
                                     std::shared_ptr<arrow::Array> *array) {
      arrow::UInt32Builder builder(arrow::default_memory_pool());
      for (auto id : ids) {
        uint32_t size;
        auto data = grn_obj_get_value_(ctx_, grn_column, id, &size);
        builder.Append(*(reinterpret_cast<const uint32_t *>(data)));
      }
      return builder.Finish(array);
    }

    arrow::Status build_int32_array(std::vector<grn_id> &ids,
                                    grn_obj *grn_column,
                                    std::shared_ptr<arrow::Array> *array) {
      arrow::Int32Builder builder(arrow::default_memory_pool());
      for (auto id : ids) {
        uint32_t size;
        auto data = grn_obj_get_value_(ctx_, grn_column, id, &size);
        builder.Append(*(reinterpret_cast<const int32_t *>(data)));
      }
      return builder.Finish(array);
    }
    arrow::Status build_uint64_array(std::vector<grn_id> &ids,
                                     grn_obj *grn_column,
                                     std::shared_ptr<arrow::Array> *array) {
      arrow::UInt64Builder builder(arrow::default_memory_pool());
      for (auto id : ids) {
        uint32_t size;
        auto data = grn_obj_get_value_(ctx_, grn_column, id, &size);
        builder.Append(*(reinterpret_cast<const uint64_t *>(data)));
      }
      return builder.Finish(array);
    }

    arrow::Status build_int64_array(std::vector<grn_id> &ids,
                                    grn_obj *grn_column,
                                    std::shared_ptr<arrow::Array> *array) {
      arrow::Int64Builder builder(arrow::default_memory_pool());
      for (auto id : ids) {
        uint32_t size;
        auto data = grn_obj_get_value_(ctx_, grn_column, id, &size);
        builder.Append(*(reinterpret_cast<const int64_t *>(data)));
      }
      return builder.Finish(array);
    }

    arrow::Status build_double_array(std::vector<grn_id> &ids,
                                     grn_obj *grn_column,
                                     std::shared_ptr<arrow::Array> *array) {
      arrow::DoubleBuilder builder(arrow::default_memory_pool());
      for (auto id : ids) {
        uint32_t size;
        auto data = grn_obj_get_value_(ctx_, grn_column, id, &size);
        builder.Append(*(reinterpret_cast<const double *>(data)));
      }
      return builder.Finish(array);
    }

    arrow::Status build_timestamp_array(std::vector<grn_id> &ids,
                                        grn_obj *grn_column,
                                        std::shared_ptr<arrow::Array> *array) {
      auto timestamp_ns_data_type =
        std::make_shared<arrow::TimestampType>(arrow::TimeUnit::MICRO);
      arrow::TimestampBuilder builder(arrow::default_memory_pool(),
                                      timestamp_ns_data_type);
      for (auto id : ids) {
        uint32_t size;
        auto data = grn_obj_get_value_(ctx_, grn_column, id, &size);
        auto timestamp_ns = *(reinterpret_cast<const int64_t *>(data));
        builder.Append(timestamp_ns);
      }
      return builder.Finish(array);
    }

    arrow::Status build_utf8_array(std::vector<grn_id> &ids,
                                   grn_obj *grn_column,
                                   std::shared_ptr<arrow::Array> *array) {
      arrow::StringBuilder builder(arrow::default_memory_pool());
      for (auto id : ids) {
        uint32_t size;
        auto data = grn_obj_get_value_(ctx_, grn_column, id, &size);
        builder.Append(data, size);
      }
      return builder.Finish(array);
    }
  };
}
#endif /* GRN_WITH_ARROW */

extern "C" {
grn_rc
grn_arrow_load(grn_ctx *ctx,
               grn_obj *table,
               const char *path)
{
  GRN_API_ENTER;
#ifdef GRN_WITH_ARROW
  std::shared_ptr<arrow::io::MemoryMappedFile> input;
  auto status =
    arrow::io::MemoryMappedFile::Open(path, arrow::io::FileMode::READ, &input);
  if (!grnarrow::check_status(ctx,
                              status,
                              std::ostringstream() <<
                              "[arrow][load] failed to open path: " <<
                              "<" << path << ">")) {
    GRN_API_RETURN(ctx->rc);
  }
  std::shared_ptr<arrow::ipc::RecordBatchFileReader> reader;
  status = arrow::ipc::RecordBatchFileReader::Open(input, &reader);
  if (!grnarrow::check_status(ctx,
                              status,
                              "[arrow][load] "
                              "failed to create file format reader")) {
    GRN_API_RETURN(ctx->rc);
  }

  grnarrow::FileLoader loader(ctx, table);
  int n_record_batches = reader->num_record_batches();
  for (int i = 0; i < n_record_batches; ++i) {
    std::shared_ptr<arrow::RecordBatch> record_batch;
    status = reader->ReadRecordBatch(i, &record_batch);
    if (!grnarrow::check_status(ctx,
                                status,
                                std::ostringstream("") <<
                                "[arrow][load] failed to get " <<
                                "the " << i << "-th " << "record")) {
      break;
    }
    loader.load_record_batch(record_batch);
    if (ctx->rc != GRN_SUCCESS) {
      break;
    }
  }
#else /* GRN_WITH_ARROW */
  ERR(GRN_FUNCTION_NOT_IMPLEMENTED,
      "[arrow][load] Apache Arrow support isn't enabled");
#endif /* GRN_WITH_ARROW */
  GRN_API_RETURN(ctx->rc);
}

grn_rc
grn_arrow_dump(grn_ctx *ctx,
               grn_obj *table,
               const char *path)
{
  GRN_API_ENTER;
#ifdef GRN_WITH_ARROW
  auto all_columns =
    grn_hash_create(ctx,
                    NULL,
                    sizeof(grn_id),
                    0,
                    GRN_OBJ_TABLE_HASH_KEY | GRN_HASH_TINY);
  grn_table_columns(ctx,
                    table,
                    "", 0,
                    reinterpret_cast<grn_obj *>(all_columns));

  grn_obj columns;
  GRN_PTR_INIT(&columns, GRN_OBJ_VECTOR, GRN_ID_NIL);
  GRN_HASH_EACH_BEGIN(ctx, all_columns, cursor, id) {
    void *key;
    grn_hash_cursor_get_key(ctx, cursor, &key);
    auto column_id = static_cast<grn_id *>(key);
    auto column = grn_ctx_at(ctx, *column_id);
    GRN_PTR_PUT(ctx, &columns, column);
  } GRN_HASH_EACH_END(ctx, cursor);
  grn_hash_close(ctx, all_columns);

  grn_arrow_dump_columns(ctx, table, &columns, path);
  GRN_OBJ_FIN(ctx, &columns);
#else /* GRN_WITH_ARROW */
  ERR(GRN_FUNCTION_NOT_IMPLEMENTED,
      "[arrow][dump] Apache Arrow support isn't enabled");
#endif /* GRN_WITH_ARROW */
  GRN_API_RETURN(ctx->rc);
}

grn_rc
grn_arrow_dump_columns(grn_ctx *ctx,
                       grn_obj *table,
                       grn_obj *columns,
                       const char *path)
{
  GRN_API_ENTER;
#ifdef GRN_WITH_ARROW
  std::shared_ptr<arrow::io::FileOutputStream> output;
  auto status = arrow::io::FileOutputStream::Open(path, &output);
  if (!grnarrow::check_status(ctx,
                              status,
                              std::stringstream() <<
                              "[arrow][dump] failed to open path: " <<
                              "<" << path << ">")) {
    GRN_API_RETURN(ctx->rc);
  }

  grnarrow::FileDumper dumper(ctx, table, columns);
  dumper.dump(output.get());
#else /* GRN_WITH_ARROW */
  ERR(GRN_FUNCTION_NOT_IMPLEMENTED,
      "[arrow][dump] Apache Arrow support isn't enabled");
#endif /* GRN_WITH_ARROW */
  GRN_API_RETURN(ctx->rc);
}
}
