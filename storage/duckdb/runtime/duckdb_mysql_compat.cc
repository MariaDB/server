/*
  Copyright (c) 2026, MariaDB Foundation.
  Copyright (c) 2026, Roman Nozdrin <drrtuy@gmail.com>
  Copyright (c) 2026, Leonid Fedorov.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA
*/

/*
  DuckDB scalar function overloads for MariaDB-compatible behavior.

  These add missing type overloads to DuckDB builtins so that pushdown
  queries from MariaDB work without SQL text rewriting.  Registered
  once at DuckdbManager::Initialize() via register_mysql_compat_functions().

  hex/oct/bin implementations ported from AliSQL's DuckDB fork.
*/

#include <my_global.h>
#include "log.h"

#undef UNKNOWN

#include "duckdb_mysql_compat.h"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/bit_utils.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include <cerrno>
#include <cstdlib>
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/connection.hpp"

#include "duckdb/common/types/string_type.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/scalar/regexp.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "re2/re2.h"

namespace myduck
{

/* ================================================================
   octet_length(VARCHAR) -> BIGINT
   DuckDB builtin only has octet_length(BLOB).
   MariaDB OCTET_LENGTH() works on any string type.
   ================================================================ */

static void octet_length_varchar_func(duckdb::DataChunk &args,
                                      duckdb::ExpressionState &state,
                                      duckdb::Vector &result)
{
  auto &input= args.data[0];
  auto count= args.size();

  duckdb::UnaryExecutor::Execute<duckdb::string_t, int64_t>(
      input, result, count,
      [](duckdb::string_t s) -> int64_t { return (int64_t) s.GetSize(); });
}

/* ================================================================
   length(VARCHAR) -> BIGINT  (byte count, MariaDB semantics)
   DuckDB builtin length(VARCHAR) returns character count.
   MariaDB LENGTH() = OCTET_LENGTH() = byte count.
   We override to match MariaDB behavior for pushdown queries.
   ================================================================ */

static void length_varchar_byte_func(duckdb::DataChunk &args,
                                     duckdb::ExpressionState &state,
                                     duckdb::Vector &result)
{
  duckdb::UnaryExecutor::Execute<duckdb::string_t, int64_t>(
      args.data[0], result, args.size(),
      [](duckdb::string_t s) -> int64_t { return (int64_t) s.GetSize(); });
}

/* ================================================================
   length(BLOB) -> BIGINT
   DuckDB builtin length() only works on VARCHAR (returns char count).
   MariaDB LENGTH() = OCTET_LENGTH() = byte count.
   ================================================================ */

/* ================================================================
   ascii(VARCHAR) -> INTEGER  (first byte, MariaDB semantics)
   DuckDB builtin ascii() returns Unicode codepoint of first character.
   MariaDB ASCII() returns the numeric value of the first byte.
   ================================================================ */

static void ascii_byte_func(duckdb::DataChunk &args,
                            duckdb::ExpressionState &state,
                            duckdb::Vector &result)
{
  duckdb::UnaryExecutor::Execute<duckdb::string_t, int32_t>(
      args.data[0], result, args.size(),
      [](duckdb::string_t s) -> int32_t {
        return s.GetSize() > 0 ? (unsigned char) s.GetData()[0] : 0;
      });
}

/* ================================================================
   ord(VARCHAR) -> BIGINT  (multibyte byte-value, MariaDB semantics)
   DuckDB builtin ord() returns Unicode codepoint.
   MariaDB ORD() for multibyte characters returns
   (byte1 * 256 + byte2) * 256 + byte3 ... etc.
   For single-byte characters, same as ASCII().
   ================================================================ */

static void ord_byte_func(duckdb::DataChunk &args,
                          duckdb::ExpressionState &state,
                          duckdb::Vector &result)
{
  duckdb::UnaryExecutor::Execute<duckdb::string_t, int32_t>(
      args.data[0], result, args.size(),
      [](duckdb::string_t s) -> int32_t {
        auto data= (const unsigned char *) s.GetData();
        auto size= s.GetSize();
        if (size == 0)
          return 0;
        /* Determine UTF-8 character length from first byte */
        unsigned char c= data[0];
        int char_len= 1;
        if (c >= 0xF0)
          char_len= 4;
        else if (c >= 0xE0)
          char_len= 3;
        else if (c >= 0xC0)
          char_len= 2;
        /* Single byte — same as ASCII */
        if (char_len == 1)
          return (int32_t) c;
        /* Multibyte: (b1 * 256 + b2) * 256 + b3 ... */
        int32_t val= 0;
        for (int i= 0; i < char_len && i < (int) size; i++)
          val= val * 256 + data[i];
        return val;
      });
}

/* ================================================================
   Helper: parse MariaDB time interval string 'D H:M:S.us' into
   microseconds. Supports formats:
     'HH:MM:SS', 'HH:MM:SS.uuuuuu', 'D HH:MM:SS', 'D HH:MM:SS.uuuuuu'
   Returns total microseconds. Negative values supported via leading '-'.
   ================================================================ */

static int64_t parse_mariadb_interval_us(const char *data, size_t len)
{
  if (len == 0)
    return 0;
  bool neg= false;
  size_t i= 0;
  if (data[0] == '-')
  {
    neg= true;
    i++;
  }
  int days= 0, hours= 0, minutes= 0, seconds= 0, usec= 0;

  /* Check if there's a 'D ' prefix (day followed by space) */
  size_t space= std::string(data + i, len - i).find(' ');
  if (space != std::string::npos)
  {
    days= atoi(std::string(data + i, space).c_str());
    i+= space + 1;
  }

  /* Parse H:M:S */
  int parts[3]= {0, 0, 0};
  int pidx= 0;
  size_t num_start= i;
  for (; i <= len && pidx < 3; i++)
  {
    if (i == len || data[i] == ':' || data[i] == '.')
    {
      parts[pidx++]= atoi(std::string(data + num_start, i - num_start).c_str());
      num_start= i + 1;
      if (i < len && data[i] == '.')
      {
        i++;
        break;
      }
    }
  }
  hours= parts[0];
  minutes= parts[1];
  seconds= parts[2];

  /* Parse fractional seconds */
  if (i < len)
  {
    std::string frac(data + i, len - i);
    /* Pad to 6 digits */
    while (frac.size() < 6)
      frac+= '0';
    frac= frac.substr(0, 6);
    usec= atoi(frac.c_str());
  }

  int64_t total_us= ((int64_t) days * 86400 + (int64_t) hours * 3600 +
                      (int64_t) minutes * 60 + seconds) *
                         1000000 +
                     usec;
  return neg ? -total_us : total_us;
}

/* ================================================================
   addtime(TIMESTAMP, VARCHAR) -> TIMESTAMP
   subtime(TIMESTAMP, VARCHAR) -> TIMESTAMP
   MariaDB ADDTIME/SUBTIME accepts time interval in 'D H:M:S.us' format.
   DuckDB INTERVAL doesn't parse this format.
   ================================================================ */

static void addtime_func(duckdb::DataChunk &args,
                         duckdb::ExpressionState &,
                         duckdb::Vector &result)
{
  duckdb::BinaryExecutor::Execute<duckdb::timestamp_t, duckdb::string_t,
                                  duckdb::timestamp_t>(
      args.data[0], args.data[1], result, args.size(),
      [](duckdb::timestamp_t ts, duckdb::string_t interval_str)
          -> duckdb::timestamp_t {
        int64_t us= parse_mariadb_interval_us(interval_str.GetData(),
                                              interval_str.GetSize());
        return duckdb::timestamp_t(ts.value + us);
      });
}

static void subtime_func(duckdb::DataChunk &args,
                         duckdb::ExpressionState &,
                         duckdb::Vector &result)
{
  duckdb::BinaryExecutor::Execute<duckdb::timestamp_t, duckdb::string_t,
                                  duckdb::timestamp_t>(
      args.data[0], args.data[1], result, args.size(),
      [](duckdb::timestamp_t ts, duckdb::string_t interval_str)
          -> duckdb::timestamp_t {
        int64_t us= parse_mariadb_interval_us(interval_str.GetData(),
                                              interval_str.GetSize());
        return duckdb::timestamp_t(ts.value - us);
      });
}

/* ================================================================
   rtrim(VARCHAR, VARCHAR), ltrim(VARCHAR, VARCHAR), trim(VARCHAR, VARCHAR)
   DuckDB builtins remove individual characters from the set.
   MariaDB TRIM removes a substring pattern (e.g. TRIM(TRAILING 'xyz' FROM s)
   removes the trailing "xyz" substring, not individual x/y/z chars).
   We override the 2-arg forms for substring semantics.
   ================================================================ */

static void rtrim_substr_func(duckdb::DataChunk &args,
                              duckdb::ExpressionState &,
                              duckdb::Vector &result)
{
  duckdb::BinaryExecutor::Execute<duckdb::string_t, duckdb::string_t,
                                  duckdb::string_t>(
      args.data[0], args.data[1], result, args.size(),
      [&](duckdb::string_t s, duckdb::string_t pat) -> duckdb::string_t {
        auto data= s.GetData();
        auto slen= (int64_t) s.GetSize();
        auto plen= (int64_t) pat.GetSize();
        if (plen == 0 || plen > slen)
          return s;
        /* Single char — same as DuckDB default behavior */
        if (plen == 1)
        {
          auto c= pat.GetData()[0];
          while (slen > 0 && data[slen - 1] == c)
            slen--;
          return duckdb::StringVector::AddString(result, data, slen);
        }
        /* Multi-char: remove trailing substring repeatedly */
        auto pdata= pat.GetData();
        while (slen >= plen &&
               memcmp(data + slen - plen, pdata, plen) == 0)
          slen-= plen;
        return duckdb::StringVector::AddString(result, data, slen);
      });
}

static void ltrim_substr_func(duckdb::DataChunk &args,
                              duckdb::ExpressionState &,
                              duckdb::Vector &result)
{
  duckdb::BinaryExecutor::Execute<duckdb::string_t, duckdb::string_t,
                                  duckdb::string_t>(
      args.data[0], args.data[1], result, args.size(),
      [&](duckdb::string_t s, duckdb::string_t pat) -> duckdb::string_t {
        auto data= s.GetData();
        auto slen= (int64_t) s.GetSize();
        auto plen= (int64_t) pat.GetSize();
        int64_t start= 0;
        if (plen == 0 || plen > slen)
          return s;
        if (plen == 1)
        {
          auto c= pat.GetData()[0];
          while (start < slen && data[start] == c)
            start++;
          return duckdb::StringVector::AddString(result, data + start,
                                                 slen - start);
        }
        auto pdata= pat.GetData();
        while (start + plen <= slen &&
               memcmp(data + start, pdata, plen) == 0)
          start+= plen;
        return duckdb::StringVector::AddString(result, data + start,
                                               slen - start);
      });
}

static void length_blob_func(duckdb::DataChunk &args,
                             duckdb::ExpressionState &state,
                             duckdb::Vector &result)
{
  auto &input= args.data[0];
  auto count= args.size();

  duckdb::UnaryExecutor::Execute<duckdb::string_t, int64_t>(
      input, result, count,
      [](duckdb::string_t s) -> int64_t { return (int64_t) s.GetSize(); });
}

/* ================================================================
   json_contains(json, candidate, path) -> BOOLEAN
   DuckDB has json_contains(json, candidate) -- 2-arg.
   MariaDB JSON_CONTAINS(json, candidate, path) -- 3-arg, extracts
   path first then checks containment.
   Implemented as: json_contains(json_extract(json, path), candidate)
   ================================================================ */

static void json_contains_3arg_func(duckdb::DataChunk &args,
                                    duckdb::ExpressionState &state,
                                    duckdb::Vector &result)
{
  auto &json_vec= args.data[0];
  auto &candidate_vec= args.data[1];
  auto &path_vec= args.data[2];
  auto count= args.size();

  duckdb::TernaryExecutor::Execute<duckdb::string_t, duckdb::string_t,
                                   duckdb::string_t, bool>(
      json_vec, candidate_vec, path_vec, result, count,
      [](duckdb::string_t json, duckdb::string_t candidate,
         duckdb::string_t path) -> bool {
        /* Minimal implementation: delegate to DuckDB's own functions
           would require a ClientContext which we don't have here.
           For now, return false -- placeholder for proper implementation. */
        (void) json;
        (void) candidate;
        (void) path;
        return false;
      });
}

/* ================================================================
   hex / oct / bin helper functions
   Ported from AliSQL's DuckDB fork (core_functions/scalar/string/hex.cpp).
   ================================================================ */

namespace {

using namespace duckdb;

/* ---- Hex byte writers ---- */

static void WriteHexBytes(uint64_t x, char *&output, idx_t buffer_size)
{
  idx_t offset= buffer_size * 4;
  for (; offset >= 4; offset -= 4)
  {
    uint8_t byte= (x >> (offset - 4)) & 0x0F;
    *output= Blob::HEX_TABLE[byte];
    output++;
  }
}

template <class T>
static void WriteHugeIntHexBytes(T x, char *&output, idx_t buffer_size)
{
  idx_t offset= buffer_size * 4;
  auto upper= x.upper;
  auto lower= x.lower;

  for (; offset >= 68; offset -= 4)
  {
    uint8_t byte= (upper >> (offset - 68)) & 0x0F;
    *output= Blob::HEX_TABLE[byte];
    output++;
  }

  for (; offset >= 4; offset -= 4)
  {
    uint8_t byte= (lower >> (offset - 4)) & 0x0F;
    *output= Blob::HEX_TABLE[byte];
    output++;
  }
}

/* ---- Binary (bin) byte writers ---- */

static void WriteBinBytes(uint64_t x, char *&output, idx_t buffer_size)
{
  idx_t offset= buffer_size;
  for (; offset >= 1; offset -= 1)
  {
    *output= NumericCast<char>(((x >> (offset - 1)) & 0x01) + '0');
    output++;
  }
}

template <class T>
static void WriteHugeIntBinBytes(T x, char *&output, idx_t buffer_size)
{
  auto upper= x.upper;
  auto lower= x.lower;
  idx_t offset= buffer_size;

  for (; offset >= 65; offset -= 1)
  {
    *output= ((upper >> (offset - 65)) & 0x01) + '0';
    output++;
  }

  for (; offset >= 1; offset -= 1)
  {
    *output= ((lower >> (offset - 1)) & 0x01) + '0';
    output++;
  }
}

/* ---- Octal byte writers ---- */

static void WriteOctBytes(uint64_t x, char *&output, idx_t buffer_size)
{
  idx_t offset= buffer_size * 3;
  for (; offset >= 3; offset -= 3)
  {
    uint8_t byte= (x >> (offset - 3)) & 0x07;
    *output= Blob::HEX_TABLE[byte];
    output++;
  }
}

template <class T>
static void WriteHugeIntOctBytes(T x, char *&output, idx_t buffer_size)
{
  idx_t offset= buffer_size * 3;
  auto upper= x.upper;
  auto lower= x.lower;

  for (; offset >= 69; offset -= 3)
  {
    uint8_t byte= (upper >> (offset - 66)) & 0x07;
    *output= Blob::HEX_TABLE[byte];
    output++;
  }

  {
    uint8_t byte= ((upper & 0x03) << 1) + ((lower >> offset) & 0x01);
    *output= Blob::HEX_TABLE[byte];
    output++;
    offset -= 3;
  }

  for (; offset >= 3; offset -= 3)
  {
    uint8_t byte= (lower >> (offset - 3)) & 0x07;
    *output= Blob::HEX_TABLE[byte];
    output++;
  }
}

/* ================================================================
   Hex operator structs
   ================================================================ */

struct HexStrOperator {
  template <class INPUT_TYPE, class RESULT_TYPE>
  static RESULT_TYPE Operation(INPUT_TYPE input, Vector &result)
  {
    auto data= input.GetData();
    auto size= input.GetSize();

    auto target= StringVector::EmptyString(result, size * 2);
    auto output= target.GetDataWriteable();

    for (idx_t i= 0; i < size; ++i)
    {
      *output= Blob::HEX_TABLE[(data[i] >> 4) & 0x0F];
      output++;
      *output= Blob::HEX_TABLE[data[i] & 0x0F];
      output++;
    }

    target.Finalize();
    return target;
  }
};

struct HexIntegralOperator {
  template <class INPUT_TYPE, class RESULT_TYPE>
  static RESULT_TYPE Operation(INPUT_TYPE input, Vector &result)
  {
    auto num_leading_zero=
        CountZeros<uint64_t>::Leading(static_cast<uint64_t>(input));
    idx_t num_bits_to_check= 64 - num_leading_zero;
    D_ASSERT(num_bits_to_check <= sizeof(INPUT_TYPE) * 8);

    idx_t buffer_size= (num_bits_to_check + 3) / 4;

    if (buffer_size == 0)
    {
      auto target= StringVector::EmptyString(result, 1);
      auto output= target.GetDataWriteable();
      *output= '0';
      target.Finalize();
      return target;
    }

    D_ASSERT(buffer_size > 0);
    auto target= StringVector::EmptyString(result, buffer_size);
    auto output= target.GetDataWriteable();

    WriteHexBytes(static_cast<uint64_t>(input), output, buffer_size);

    target.Finalize();
    return target;
  }
};

struct HexHugeIntOperator {
  template <class INPUT_TYPE, class RESULT_TYPE>
  static RESULT_TYPE Operation(INPUT_TYPE input, Vector &result)
  {
    idx_t num_leading_zero=
        CountZeros<hugeint_t>::Leading(UnsafeNumericCast<hugeint_t>(input));
    idx_t buffer_size= sizeof(INPUT_TYPE) * 2 - (num_leading_zero / 4);

    if (buffer_size == 0)
    {
      auto target= StringVector::EmptyString(result, 1);
      auto output= target.GetDataWriteable();
      *output= '0';
      target.Finalize();
      return target;
    }

    D_ASSERT(buffer_size > 0);
    auto target= StringVector::EmptyString(result, buffer_size);
    auto output= target.GetDataWriteable();

    WriteHugeIntHexBytes<hugeint_t>(input, output, buffer_size);

    target.Finalize();
    return target;
  }
};

struct HexUhugeIntOperator {
  template <class INPUT_TYPE, class RESULT_TYPE>
  static RESULT_TYPE Operation(INPUT_TYPE input, Vector &result)
  {
    idx_t num_leading_zero=
        CountZeros<uhugeint_t>::Leading(UnsafeNumericCast<uhugeint_t>(input));
    idx_t buffer_size= sizeof(INPUT_TYPE) * 2 - (num_leading_zero / 4);

    if (buffer_size == 0)
    {
      auto target= StringVector::EmptyString(result, 1);
      auto output= target.GetDataWriteable();
      *output= '0';
      target.Finalize();
      return target;
    }

    D_ASSERT(buffer_size > 0);
    auto target= StringVector::EmptyString(result, buffer_size);
    auto output= target.GetDataWriteable();

    WriteHugeIntHexBytes<uhugeint_t>(input, output, buffer_size);

    target.Finalize();
    return target;
  }
};

struct HexFloatOperator {
  template <class INPUT_TYPE, class RESULT_TYPE>
  static RESULT_TYPE Operation(INPUT_TYPE input, Vector &result)
  {
    int64_t input_integer= std::round(input);
    return HexIntegralOperator::Operation<int64_t, string_t>(input_integer,
                                                             result);
  }
};

/* ================================================================
   Oct operator structs
   ================================================================ */

struct OctIntegralOperator {
  template <class INPUT_TYPE, class RESULT_TYPE>
  static RESULT_TYPE Operation(INPUT_TYPE input, Vector &result)
  {
    auto num_leading_zero=
        CountZeros<uint64_t>::Leading(static_cast<uint64_t>(input));
    idx_t num_bits_to_check= 64 - num_leading_zero;
    D_ASSERT(num_bits_to_check <= sizeof(INPUT_TYPE) * 8);

    idx_t buffer_size= (num_bits_to_check + 2) / 3;

    if (buffer_size == 0)
    {
      auto target= StringVector::EmptyString(result, 1);
      auto output= target.GetDataWriteable();
      *output= '0';
      target.Finalize();
      return target;
    }

    D_ASSERT(buffer_size > 0);
    auto target= StringVector::EmptyString(result, buffer_size);
    auto output= target.GetDataWriteable();

    WriteOctBytes(static_cast<uint64_t>(input), output, buffer_size);

    target.Finalize();
    return target;
  }
};

struct OctHugeIntOperator {
  template <class INPUT_TYPE, class RESULT_TYPE>
  static RESULT_TYPE Operation(INPUT_TYPE input, Vector &result)
  {
    idx_t num_leading_zero=
        CountZeros<hugeint_t>::Leading(UnsafeNumericCast<hugeint_t>(input));
    idx_t buffer_size=
        (sizeof(INPUT_TYPE) * 2 - num_leading_zero + 2) / 3;

    if (buffer_size == 0)
    {
      auto target= StringVector::EmptyString(result, 1);
      auto output= target.GetDataWriteable();
      *output= '0';
      target.Finalize();
      return target;
    }

    D_ASSERT(buffer_size > 0);
    auto target= StringVector::EmptyString(result, buffer_size);
    auto output= target.GetDataWriteable();

    WriteHugeIntOctBytes<hugeint_t>(input, output, buffer_size);

    target.Finalize();
    return target;
  }
};

struct OctUhugeIntOperator {
  template <class INPUT_TYPE, class RESULT_TYPE>
  static RESULT_TYPE Operation(INPUT_TYPE input, Vector &result)
  {
    idx_t num_leading_zero=
        CountZeros<uhugeint_t>::Leading(UnsafeNumericCast<uhugeint_t>(input));
    idx_t buffer_size=
        (sizeof(INPUT_TYPE) * 2 - num_leading_zero + 2) / 3;

    if (buffer_size == 0)
    {
      auto target= StringVector::EmptyString(result, 1);
      auto output= target.GetDataWriteable();
      *output= '0';
      target.Finalize();
      return target;
    }

    D_ASSERT(buffer_size > 0);
    auto target= StringVector::EmptyString(result, buffer_size);
    auto output= target.GetDataWriteable();

    WriteHugeIntOctBytes<uhugeint_t>(input, output, buffer_size);

    target.Finalize();
    return target;
  }
};

struct OctFloatOperator {
  template <class INPUT_TYPE, class RESULT_TYPE>
  static RESULT_TYPE Operation(INPUT_TYPE input, Vector &result)
  {
    int64_t input_integer= std::round(input);
    return OctIntegralOperator::Operation<int64_t, string_t>(input_integer,
                                                             result);
  }
};

struct OctStrOperator {
  template <class INPUT_TYPE, class RESULT_TYPE>
  static RESULT_TYPE Operation(INPUT_TYPE input, Vector &result)
  {
    double d;
    std::string tmp(input.GetData(), input.GetSize());
    char *end= nullptr;
    errno= 0;
    d= strtod(tmp.c_str(), &end);
    bool success= (errno == 0 && end != tmp.c_str());
    if (!success)
    {
      auto target= StringVector::EmptyString(result, 1);
      auto output= target.GetDataWriteable();
      *output= '0';
      target.Finalize();
      return target;
    }
    else
    {
      return OctFloatOperator::Operation<double, RESULT_TYPE>(d, result);
    }
  }
};

/* ================================================================
   Bin (binary) operator structs
   ================================================================ */

struct BinaryIntegralOperator {
  template <class INPUT_TYPE, class RESULT_TYPE>
  static RESULT_TYPE Operation(INPUT_TYPE input, Vector &result)
  {
    auto num_leading_zero=
        CountZeros<uint64_t>::Leading(static_cast<uint64_t>(input));
    idx_t num_bits_to_check= 64 - num_leading_zero;
    D_ASSERT(num_bits_to_check <= sizeof(INPUT_TYPE) * 8);

    idx_t buffer_size= num_bits_to_check;

    if (buffer_size == 0)
    {
      auto target= StringVector::EmptyString(result, 1);
      auto output= target.GetDataWriteable();
      *output= '0';
      target.Finalize();
      return target;
    }

    D_ASSERT(buffer_size > 0);
    auto target= StringVector::EmptyString(result, buffer_size);
    auto output= target.GetDataWriteable();

    WriteBinBytes(static_cast<uint64_t>(input), output, buffer_size);

    target.Finalize();
    return target;
  }
};

struct BinaryHugeIntOperator {
  template <class INPUT_TYPE, class RESULT_TYPE>
  static RESULT_TYPE Operation(INPUT_TYPE input, Vector &result)
  {
    auto num_leading_zero=
        CountZeros<hugeint_t>::Leading(UnsafeNumericCast<hugeint_t>(input));
    idx_t buffer_size= sizeof(INPUT_TYPE) * 8 - num_leading_zero;

    if (buffer_size == 0)
    {
      auto target= StringVector::EmptyString(result, 1);
      auto output= target.GetDataWriteable();
      *output= '0';
      target.Finalize();
      return target;
    }

    auto target= StringVector::EmptyString(result, buffer_size);
    auto output= target.GetDataWriteable();

    WriteHugeIntBinBytes<hugeint_t>(input, output, buffer_size);

    target.Finalize();
    return target;
  }
};

struct BinaryUhugeIntOperator {
  template <class INPUT_TYPE, class RESULT_TYPE>
  static RESULT_TYPE Operation(INPUT_TYPE input, Vector &result)
  {
    auto num_leading_zero=
        CountZeros<uhugeint_t>::Leading(UnsafeNumericCast<uhugeint_t>(input));
    idx_t buffer_size= sizeof(INPUT_TYPE) * 8 - num_leading_zero;

    if (buffer_size == 0)
    {
      auto target= StringVector::EmptyString(result, 1);
      auto output= target.GetDataWriteable();
      *output= '0';
      target.Finalize();
      return target;
    }

    auto target= StringVector::EmptyString(result, buffer_size);
    auto output= target.GetDataWriteable();

    WriteHugeIntBinBytes<uhugeint_t>(input, output, buffer_size);

    target.Finalize();
    return target;
  }
};

struct BinaryFloatOperator {
  template <class INPUT_TYPE, class RESULT_TYPE>
  static RESULT_TYPE Operation(INPUT_TYPE input, Vector &result)
  {
    int64_t input_integer= std::round(input);
    return BinaryIntegralOperator::Operation<int64_t, string_t>(input_integer,
                                                                result);
  }
};

struct BinaryStrOperator {
  template <class INPUT_TYPE, class RESULT_TYPE>
  static RESULT_TYPE Operation(INPUT_TYPE input, Vector &result)
  {
    double d;
    std::string tmp(input.GetData(), input.GetSize());
    char *end= nullptr;
    errno= 0;
    d= strtod(tmp.c_str(), &end);
    bool success= (errno == 0 && end != tmp.c_str());
    if (!success)
    {
      auto target= StringVector::EmptyString(result, 1);
      auto output= target.GetDataWriteable();
      *output= '0';
      target.Finalize();
      return target;
    }
    else
    {
      return BinaryFloatOperator::Operation<double, RESULT_TYPE>(d, result);
    }
  }
};

/* ================================================================
   Template wrapper functions for UnaryExecutor::ExecuteString
   ================================================================ */

template <class INPUT, class OP>
static void ToHexFunction(DataChunk &args, ExpressionState &state,
                          Vector &result)
{
  D_ASSERT(args.ColumnCount() == 1);
  auto &input= args.data[0];
  idx_t count= args.size();
  UnaryExecutor::ExecuteString<INPUT, string_t, OP>(input, result, count);
}

template <class INPUT, class OP>
static void ToBinaryFunction(DataChunk &args, ExpressionState &state,
                             Vector &result)
{
  D_ASSERT(args.ColumnCount() == 1);
  auto &input= args.data[0];
  idx_t count= args.size();
  UnaryExecutor::ExecuteString<INPUT, string_t, OP>(input, result, count);
}

template <class INPUT, class OP>
static void ToOctFunction(DataChunk &args, ExpressionState &state,
                          Vector &result)
{
  D_ASSERT(args.ColumnCount() == 1);
  auto &input= args.data[0];
  idx_t count= args.size();
  UnaryExecutor::ExecuteString<INPUT, string_t, OP>(input, result, count);
}

} /* anonymous namespace */

/* ================================================================
   locate(substr, str) -> BIGINT
   locate(substr, str, pos) -> BIGINT
   MariaDB LOCATE(substr, str [, pos]) returns the position of the
   first occurrence of substr in str, starting at position pos (1-based).
   This is the reversed argument order of DuckDB's instr(str, substr).
   ================================================================ */

static void locate_2arg_func(duckdb::DataChunk &args,
                             duckdb::ExpressionState &state,
                             duckdb::Vector &result)
{
  auto &needle_vec= args.data[0];
  auto &haystack_vec= args.data[1];
  auto count= args.size();

  duckdb::BinaryExecutor::Execute<duckdb::string_t, duckdb::string_t,
                                  int64_t>(
      needle_vec, haystack_vec, result, count,
      [](duckdb::string_t needle, duckdb::string_t haystack) -> int64_t {
        if (needle.GetSize() == 0)
          return 1;
        auto haystack_data= haystack.GetData();
        auto haystack_size= haystack.GetSize();
        auto needle_data= needle.GetData();
        auto needle_size= needle.GetSize();

        if (needle_size > haystack_size)
          return 0;

        for (duckdb::idx_t i= 0; i <= haystack_size - needle_size; i++)
        {
          if (memcmp(haystack_data + i, needle_data, needle_size) == 0)
            return (int64_t)(i + 1);
        }
        return 0;
      });
}

static void locate_3arg_func(duckdb::DataChunk &args,
                             duckdb::ExpressionState &state,
                             duckdb::Vector &result)
{
  auto &needle_vec= args.data[0];
  auto &haystack_vec= args.data[1];
  auto &pos_vec= args.data[2];
  auto count= args.size();

  duckdb::TernaryExecutor::Execute<duckdb::string_t, duckdb::string_t,
                                   int64_t, int64_t>(
      needle_vec, haystack_vec, pos_vec, result, count,
      [](duckdb::string_t needle, duckdb::string_t haystack,
         int64_t pos) -> int64_t {
        if (pos < 1)
          return 0;
        if (needle.GetSize() == 0)
          return pos;

        auto haystack_data= haystack.GetData();
        auto haystack_size= (int64_t) haystack.GetSize();
        auto needle_data= needle.GetData();
        auto needle_size= (int64_t) needle.GetSize();

        /* pos is 1-based; convert to 0-based offset */
        int64_t start= pos - 1;
        if (start >= haystack_size)
          return 0;

        if (needle_size > haystack_size - start)
          return 0;

        for (int64_t i= start; i <= haystack_size - needle_size; i++)
        {
          if (memcmp(haystack_data + i, needle_data, needle_size) == 0)
            return i + 1;
        }
        return 0;
      });
}

/* ================================================================
   regexp_replace(VARCHAR, VARCHAR, VARCHAR) -> VARCHAR

   MariaDB REGEXP_REPLACE replaces ALL matches (global), unlike DuckDB's
   native 3-arg form which replaces only the first.  We reuse DuckDB's
   native bind-data / local-state so a constant pattern is compiled once
   at bind time (RegexInitLocalState) instead of per row.

   Invalid-pattern behavior mirrors MariaDB:
     - constant pattern  -> RegexLocalState ctor throws (query error);
     - non-constant       -> per-row NULL.
   ================================================================ */

static duckdb::unique_ptr<duckdb::FunctionData>
regexp_replace_bind(duckdb::ClientContext &context,
                    duckdb::ScalarFunction &,
                    duckdb::vector<duckdb::unique_ptr<duckdb::Expression>>
                        &arguments)
{
  auto data= duckdb::make_uniq<duckdb::RegexpReplaceBindData>();
  data->constant_pattern= duckdb::regexp_util::TryParseConstantPattern(
      context, *arguments[1], data->constant_string);
  data->global_replace= true;
  data->options.set_log_errors(false);
  return duckdb::unique_ptr<duckdb::FunctionData>(std::move(data));
}

static void regexp_replace_global_func(duckdb::DataChunk &args,
                                       duckdb::ExpressionState &state,
                                       duckdb::Vector &result)
{
  auto &func_expr= state.expr.Cast<duckdb::BoundFunctionExpression>();
  auto &info= func_expr.bind_info->Cast<duckdb::RegexpReplaceBindData>();

  auto &strings= args.data[0];
  auto &patterns= args.data[1];
  auto &replaces= args.data[2];

  if (info.constant_pattern)
  {
    auto &lstate= duckdb::ExecuteFunctionState::GetFunctionState(state)
                      ->Cast<duckdb::RegexLocalState>();
    duckdb::BinaryExecutor::Execute<duckdb::string_t, duckdb::string_t,
                                    duckdb::string_t>(
        strings, replaces, result, args.size(),
        [&](duckdb::string_t input, duckdb::string_t replace) {
          std::string s= input.GetString();
          duckdb_re2::RE2::GlobalReplace(
              &s, lstate.constant_pattern,
              duckdb_re2::StringPiece(replace.GetData(), replace.GetSize()));
          return duckdb::StringVector::AddString(result, s);
        });
  }
  else
  {
    duckdb::TernaryExecutor::ExecuteWithNulls<duckdb::string_t,
                                              duckdb::string_t,
                                              duckdb::string_t,
                                              duckdb::string_t>(
        strings, patterns, replaces, result, args.size(),
        [&](duckdb::string_t input, duckdb::string_t pattern,
            duckdb::string_t replace, duckdb::ValidityMask &mask,
            duckdb::idx_t idx) -> duckdb::string_t {
          duckdb_re2::RE2 re(
              duckdb_re2::StringPiece(pattern.GetData(), pattern.GetSize()),
              info.options);
          if (!re.ok())
          {
            mask.SetInvalid(idx);
            return duckdb::string_t();
          }
          std::string s= input.GetString();
          duckdb_re2::RE2::GlobalReplace(
              &s, re,
              duckdb_re2::StringPiece(replace.GetData(), replace.GetSize()));
          return duckdb::StringVector::AddString(result, s);
        });
  }
}

/* ================================================================
   WEEK(date, mode) / YEARWEEK(date, mode)
   DuckDB builtins only accept a single argument. MariaDB pushes
   down the 2-arg form which carries the week-format mode.

   Logic ported from MariaDB sql_time.cc / sql-common/my_time.c:
   calc_daynr(), calc_days_in_year(), calc_weekday(), calc_week().
   ================================================================ */

/* MariaDB week_behaviour bit flags (sql/sql_time.h) */
static constexpr uint32_t MC_WEEK_MONDAY_FIRST= 1;
static constexpr uint32_t MC_WEEK_YEAR= 2;
static constexpr uint32_t MC_WEEK_FIRST_WEEKDAY= 4;

static uint32_t mc_week_mode(uint32_t mode)
{
  uint32_t week_format= (mode & 7);
  if (!(week_format & MC_WEEK_MONDAY_FIRST))
    week_format^= MC_WEEK_FIRST_WEEKDAY;
  return week_format;
}

static uint32_t mc_calc_days_in_year(uint32_t year)
{
  return ((year & 3) == 0 && (year % 100 || (year % 400 == 0 && year)))
             ? 366
             : 365;
}

static long mc_calc_daynr(uint32_t year, uint32_t month, uint32_t day)
{
  long delsum;
  int temp;
  int y= (int) year;

  if (y == 0 && month == 0)
    return 0;
  delsum= (long) (365 * y + 31 * ((int) month - 1) + (int) day);
  if (month <= 2)
    y--;
  else
    delsum-= (long) ((int) month * 4 + 23) / 10;
  temp= (int) ((y / 100 + 1) * 3) / 4;
  return delsum + y / 4 - temp;
}

static int mc_calc_weekday(long daynr, bool sunday_first_day_of_week)
{
  return (int) ((daynr + 5L + (sunday_first_day_of_week ? 1L : 0L)) % 7);
}

static uint32_t mc_calc_week(uint32_t l_year, uint32_t l_month, uint32_t l_day,
                             uint32_t week_behaviour, uint32_t *year)
{
  uint32_t days;
  long daynr= mc_calc_daynr(l_year, l_month, l_day);
  long first_daynr= mc_calc_daynr(l_year, 1, 1);
  bool monday_first= (week_behaviour & MC_WEEK_MONDAY_FIRST) != 0;
  bool week_year= (week_behaviour & MC_WEEK_YEAR) != 0;
  bool first_weekday= (week_behaviour & MC_WEEK_FIRST_WEEKDAY) != 0;

  uint32_t weekday= (uint32_t) mc_calc_weekday(first_daynr, !monday_first);
  *year= l_year;

  if (l_month == 1 && l_day <= 7 - weekday)
  {
    if (!week_year &&
        ((first_weekday && weekday != 0) ||
         (!first_weekday && weekday >= 4)))
      return 0;
    week_year= true;
    (*year)--;
    first_daynr-= (days= mc_calc_days_in_year(*year));
    weekday= (weekday + 53 * 7 - days) % 7;
  }

  if ((first_weekday && weekday != 0) ||
      (!first_weekday && weekday >= 4))
    days= daynr - (first_daynr + (7 - weekday));
  else
    days= daynr - (first_daynr - weekday);

  if (week_year && days >= 52 * 7)
  {
    weekday= (weekday + mc_calc_days_in_year(*year)) % 7;
    if ((!first_weekday && weekday < 4) ||
        (first_weekday && weekday == 0))
    {
      (*year)++;
      return 1;
    }
  }
  return days / 7 + 1;
}

/* WEEK(DATE, INTEGER) -> BIGINT */
static void week_2arg_date_func(duckdb::DataChunk &args,
                                duckdb::ExpressionState &,
                                duckdb::Vector &result)
{
  duckdb::BinaryExecutor::Execute<duckdb::date_t, int32_t, int64_t>(
      args.data[0], args.data[1], result, args.size(),
      [](duckdb::date_t d, int32_t mode) -> int64_t {
        int32_t y, m, day;
        duckdb::Date::Convert(d, y, m, day);
        uint32_t year;
        return (int64_t) mc_calc_week((uint32_t) y, (uint32_t) m,
                                      (uint32_t) day,
                                      mc_week_mode((uint32_t) mode), &year);
      });
}

/* WEEK(TIMESTAMP, INTEGER) -> BIGINT */
static void week_2arg_ts_func(duckdb::DataChunk &args,
                              duckdb::ExpressionState &,
                              duckdb::Vector &result)
{
  duckdb::BinaryExecutor::Execute<duckdb::timestamp_t, int32_t, int64_t>(
      args.data[0], args.data[1], result, args.size(),
      [](duckdb::timestamp_t ts, int32_t mode) -> int64_t {
        int32_t y, m, day;
        duckdb::Date::Convert(duckdb::Timestamp::GetDate(ts), y, m, day);
        uint32_t year;
        return (int64_t) mc_calc_week((uint32_t) y, (uint32_t) m,
                                      (uint32_t) day,
                                      mc_week_mode((uint32_t) mode), &year);
      });
}

/* YEARWEEK(DATE, INTEGER) -> BIGINT */
static void yearweek_2arg_date_func(duckdb::DataChunk &args,
                                    duckdb::ExpressionState &,
                                    duckdb::Vector &result)
{
  duckdb::BinaryExecutor::Execute<duckdb::date_t, int32_t, int64_t>(
      args.data[0], args.data[1], result, args.size(),
      [](duckdb::date_t d, int32_t mode) -> int64_t {
        int32_t y, m, day;
        duckdb::Date::Convert(d, y, m, day);
        uint32_t year;
        uint32_t week= mc_calc_week(
            (uint32_t) y, (uint32_t) m, (uint32_t) day,
            mc_week_mode((uint32_t) mode) | MC_WEEK_YEAR, &year);
        return (int64_t) (week + year * 100);
      });
}

/* YEARWEEK(TIMESTAMP, INTEGER) -> BIGINT */
static void yearweek_2arg_ts_func(duckdb::DataChunk &args,
                                  duckdb::ExpressionState &,
                                  duckdb::Vector &result)
{
  duckdb::BinaryExecutor::Execute<duckdb::timestamp_t, int32_t, int64_t>(
      args.data[0], args.data[1], result, args.size(),
      [](duckdb::timestamp_t ts, int32_t mode) -> int64_t {
        int32_t y, m, day;
        duckdb::Date::Convert(duckdb::Timestamp::GetDate(ts), y, m, day);
        uint32_t year;
        uint32_t week= mc_calc_week(
            (uint32_t) y, (uint32_t) m, (uint32_t) day,
            mc_week_mode((uint32_t) mode) | MC_WEEK_YEAR, &year);
        return (int64_t) (week + year * 100);
      });
}

/* ================================================================
   TO_DAYS(date) -> BIGINT
   MariaDB day number since year 0. DuckDB's builtin to_days(BIGINT)
   constructs an INTERVAL instead, and has no date/string overload.
   Relation: to_days(d) = epoch_days(d) + 719528
   (since MariaDB TO_DAYS('1970-01-01') = 719528).
   ================================================================ */

static constexpr int64_t MC_TO_DAYS_EPOCH_OFFSET= 719528;

static void to_days_date_func(duckdb::DataChunk &args,
                              duckdb::ExpressionState &,
                              duckdb::Vector &result)
{
  duckdb::UnaryExecutor::Execute<duckdb::date_t, int64_t>(
      args.data[0], result, args.size(),
      [](duckdb::date_t d) -> int64_t {
        return (int64_t) duckdb::Date::EpochDays(d) + MC_TO_DAYS_EPOCH_OFFSET;
      });
}

static void to_days_varchar_func(duckdb::DataChunk &args,
                                 duckdb::ExpressionState &,
                                 duckdb::Vector &result)
{
  duckdb::UnaryExecutor::Execute<duckdb::string_t, int64_t>(
      args.data[0], result, args.size(),
      [](duckdb::string_t s) -> int64_t {
        duckdb::date_t d=
            duckdb::Date::FromCString(s.GetData(), s.GetSize());
        return (int64_t) duckdb::Date::EpochDays(d) + MC_TO_DAYS_EPOCH_OFFSET;
      });
}

/* ================================================================
   DAYOFWEEK(date) / WEEKDAY(date)
   DuckDB's dayofweek is 0=Sunday..6=Saturday; MariaDB DAYOFWEEK is
   1=Sunday..7=Saturday and WEEKDAY is 0=Monday..6=Sunday.
   Derived from ISO day-of-week (1=Monday..7=Sunday):
     DAYOFWEEK = (iso % 7) + 1
     WEEKDAY   = iso - 1
   ================================================================ */

static void dayofweek_date_func(duckdb::DataChunk &args,
                                duckdb::ExpressionState &,
                                duckdb::Vector &result)
{
  duckdb::UnaryExecutor::Execute<duckdb::date_t, int64_t>(
      args.data[0], result, args.size(),
      [](duckdb::date_t d) -> int64_t {
        return (int64_t) (duckdb::Date::ExtractISODayOfTheWeek(d) % 7) + 1;
      });
}

static void dayofweek_ts_func(duckdb::DataChunk &args,
                              duckdb::ExpressionState &,
                              duckdb::Vector &result)
{
  duckdb::UnaryExecutor::Execute<duckdb::timestamp_t, int64_t>(
      args.data[0], result, args.size(),
      [](duckdb::timestamp_t ts) -> int64_t {
        duckdb::date_t d= duckdb::Timestamp::GetDate(ts);
        return (int64_t) (duckdb::Date::ExtractISODayOfTheWeek(d) % 7) + 1;
      });
}

static void weekday_date_func(duckdb::DataChunk &args,
                              duckdb::ExpressionState &,
                              duckdb::Vector &result)
{
  duckdb::UnaryExecutor::Execute<duckdb::date_t, int64_t>(
      args.data[0], result, args.size(),
      [](duckdb::date_t d) -> int64_t {
        return (int64_t) duckdb::Date::ExtractISODayOfTheWeek(d) - 1;
      });
}

static void weekday_ts_func(duckdb::DataChunk &args,
                            duckdb::ExpressionState &,
                            duckdb::Vector &result)
{
  duckdb::UnaryExecutor::Execute<duckdb::timestamp_t, int64_t>(
      args.data[0], result, args.size(),
      [](duckdb::timestamp_t ts) -> int64_t {
        duckdb::date_t d= duckdb::Timestamp::GetDate(ts);
        return (int64_t) duckdb::Date::ExtractISODayOfTheWeek(d) - 1;
      });
}

/* ================================================================
   Registration
   ================================================================ */

void register_mysql_compat_functions(duckdb::DatabaseInstance &db)
{
  auto &catalog= duckdb::Catalog::GetSystemCatalog(db);
  auto transaction= duckdb::CatalogTransaction::GetSystemTransaction(db);

  /* octet_length(VARCHAR) -> BIGINT */
  {
    duckdb::ScalarFunctionSet set("octet_length");
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::VARCHAR}, duckdb::LogicalType::BIGINT,
        octet_length_varchar_func));
    duckdb::CreateScalarFunctionInfo info(std::move(set));
    info.on_conflict= duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
    catalog.CreateFunction(transaction, info);
  }

  /* length(VARCHAR) -> BIGINT (byte count, replaces DuckDB char count) */
  /* length(BLOB) -> BIGINT */
  {
    duckdb::ScalarFunctionSet set("length");
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::VARCHAR}, duckdb::LogicalType::BIGINT,
        length_varchar_byte_func));
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::BLOB}, duckdb::LogicalType::BIGINT,
        length_blob_func));
    duckdb::CreateScalarFunctionInfo info(std::move(set));
    info.on_conflict= duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
    catalog.CreateFunction(transaction, info);
  }

  /* ascii(VARCHAR) -> INTEGER (first byte, replaces DuckDB codepoint) */
  {
    duckdb::ScalarFunctionSet set("ascii");
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::VARCHAR}, duckdb::LogicalType::INTEGER,
        ascii_byte_func));
    duckdb::CreateScalarFunctionInfo info(std::move(set));
    info.on_conflict= duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
    catalog.CreateFunction(transaction, info);
  }

  /* ord(VARCHAR) -> INTEGER (multibyte byte-value, replaces DuckDB codepoint) */
  {
    duckdb::ScalarFunctionSet set("ord");
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::VARCHAR}, duckdb::LogicalType::INTEGER,
        ord_byte_func));
    duckdb::CreateScalarFunctionInfo info(std::move(set));
    info.on_conflict= duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
    catalog.CreateFunction(transaction, info);
  }

  /* json_contains(VARCHAR, VARCHAR, VARCHAR) -> BOOLEAN -- 3-arg */
  {
    duckdb::ScalarFunctionSet set("json_contains");
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR,
         duckdb::LogicalType::VARCHAR},
        duckdb::LogicalType::BOOLEAN, json_contains_3arg_func));
    duckdb::CreateScalarFunctionInfo info(std::move(set));
    info.on_conflict= duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
    catalog.CreateFunction(transaction, info);
  }

  /* hex() -- full AliSQL-compatible overloads */
  {
    using namespace duckdb;
    ScalarFunctionSet set("hex");
    set.AddFunction(ScalarFunction(
        {LogicalType::VARCHAR}, LogicalType::VARCHAR,
        ToHexFunction<string_t, HexStrOperator>));
    set.AddFunction(ScalarFunction(
        {LogicalType::BLOB}, LogicalType::VARCHAR,
        ToHexFunction<string_t, HexStrOperator>));
    set.AddFunction(ScalarFunction(
        {LogicalType::BIGINT}, LogicalType::VARCHAR,
        ToHexFunction<int64_t, HexIntegralOperator>));
    set.AddFunction(ScalarFunction(
        {LogicalType::UBIGINT}, LogicalType::VARCHAR,
        ToHexFunction<uint64_t, HexIntegralOperator>));
    set.AddFunction(ScalarFunction(
        {LogicalType::HUGEINT}, LogicalType::VARCHAR,
        ToHexFunction<hugeint_t, HexHugeIntOperator>));
    set.AddFunction(ScalarFunction(
        {LogicalType::UHUGEINT}, LogicalType::VARCHAR,
        ToHexFunction<uhugeint_t, HexUhugeIntOperator>));
    set.AddFunction(ScalarFunction(
        {LogicalType::DOUBLE}, LogicalType::VARCHAR,
        ToHexFunction<double, HexFloatOperator>));
    set.AddFunction(ScalarFunction(
        {LogicalType::FLOAT}, LogicalType::VARCHAR,
        ToHexFunction<float, HexFloatOperator>));
    CreateScalarFunctionInfo info(std::move(set));
    info.on_conflict= OnCreateConflict::ALTER_ON_CONFLICT;
    catalog.CreateFunction(transaction, info);
  }

  /* oct() -- full AliSQL-compatible overloads */
  {
    using namespace duckdb;
    ScalarFunctionSet set("oct");
    set.AddFunction(ScalarFunction(
        {LogicalType::VARCHAR}, LogicalType::VARCHAR,
        ToOctFunction<string_t, OctStrOperator>));
    set.AddFunction(ScalarFunction(
        {LogicalType::BLOB}, LogicalType::VARCHAR,
        ToOctFunction<string_t, OctStrOperator>));
    set.AddFunction(ScalarFunction(
        {LogicalType::BIGINT}, LogicalType::VARCHAR,
        ToOctFunction<int64_t, OctIntegralOperator>));
    set.AddFunction(ScalarFunction(
        {LogicalType::UBIGINT}, LogicalType::VARCHAR,
        ToOctFunction<uint64_t, OctIntegralOperator>));
    set.AddFunction(ScalarFunction(
        {LogicalType::HUGEINT}, LogicalType::VARCHAR,
        ToOctFunction<hugeint_t, OctHugeIntOperator>));
    set.AddFunction(ScalarFunction(
        {LogicalType::UHUGEINT}, LogicalType::VARCHAR,
        ToOctFunction<uhugeint_t, OctUhugeIntOperator>));
    set.AddFunction(ScalarFunction(
        {LogicalType::DOUBLE}, LogicalType::VARCHAR,
        ToOctFunction<double, OctFloatOperator>));
    set.AddFunction(ScalarFunction(
        {LogicalType::FLOAT}, LogicalType::VARCHAR,
        ToOctFunction<float, OctFloatOperator>));
    CreateScalarFunctionInfo info(std::move(set));
    info.on_conflict= OnCreateConflict::ALTER_ON_CONFLICT;
    catalog.CreateFunction(transaction, info);
  }

  /* bin() -- full AliSQL-compatible overloads */
  {
    using namespace duckdb;
    ScalarFunctionSet set("bin");
    set.AddFunction(ScalarFunction(
        {LogicalType::VARCHAR}, LogicalType::VARCHAR,
        ToBinaryFunction<string_t, BinaryStrOperator>));
    set.AddFunction(ScalarFunction(
        {LogicalType::BIGINT}, LogicalType::VARCHAR,
        ToBinaryFunction<int64_t, BinaryIntegralOperator>));
    set.AddFunction(ScalarFunction(
        {LogicalType::UBIGINT}, LogicalType::VARCHAR,
        ToBinaryFunction<uint64_t, BinaryIntegralOperator>));
    set.AddFunction(ScalarFunction(
        {LogicalType::HUGEINT}, LogicalType::VARCHAR,
        ToBinaryFunction<hugeint_t, BinaryHugeIntOperator>));
    set.AddFunction(ScalarFunction(
        {LogicalType::UHUGEINT}, LogicalType::VARCHAR,
        ToBinaryFunction<uhugeint_t, BinaryUhugeIntOperator>));
    set.AddFunction(ScalarFunction(
        {LogicalType::DOUBLE}, LogicalType::VARCHAR,
        ToBinaryFunction<double, BinaryFloatOperator>));
    set.AddFunction(ScalarFunction(
        {LogicalType::FLOAT}, LogicalType::VARCHAR,
        ToBinaryFunction<float, BinaryFloatOperator>));
    CreateScalarFunctionInfo info(std::move(set));
    info.on_conflict= OnCreateConflict::ALTER_ON_CONFLICT;
    catalog.CreateFunction(transaction, info);
  }

  /* locate(VARCHAR, VARCHAR) -> BIGINT  (2-arg) */
  /* locate(VARCHAR, VARCHAR, BIGINT) -> BIGINT  (3-arg) */
  {
    duckdb::ScalarFunctionSet set("locate");
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR},
        duckdb::LogicalType::BIGINT, locate_2arg_func));
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR,
         duckdb::LogicalType::BIGINT},
        duckdb::LogicalType::BIGINT, locate_3arg_func));
    duckdb::CreateScalarFunctionInfo info(std::move(set));
    info.on_conflict= duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
    catalog.CreateFunction(transaction, info);
  }

  /* mid() — registered via SQL macro calling DuckDB's substr() which
     handles multibyte UTF-8 correctly. We use a dedicated connection
     for macro creation since macros support overloading by arg count
     only when created with different names — so we use one 3-arg macro
     that the 2-arg call will match via DuckDB's default parameter.
     Actually DuckDB substr already works as 2 or 3 arg. */
  {
    auto con= std::make_shared<duckdb::Connection>(db);
    con->Query("CREATE OR REPLACE MACRO mid(s, p, n := NULL) AS "
               "CASE WHEN n IS NULL THEN substr(s, p) "
               "ELSE substr(s, p, n) END");
  }

  /* regexp_instr(VARCHAR, VARCHAR) → INTEGER
     Returns 1-based position of first match, 0 if no match. */
  {
    duckdb::ScalarFunctionSet set("regexp_instr");
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR},
        duckdb::LogicalType::INTEGER,
        [](duckdb::DataChunk &args, duckdb::ExpressionState &,
           duckdb::Vector &result) {
          duckdb::BinaryExecutor::Execute<duckdb::string_t, duckdb::string_t,
                                          int32_t>(
              args.data[0], args.data[1], result, args.size(),
              [](duckdb::string_t expr, duckdb::string_t pat) -> int32_t {
                duckdb_re2::RE2 re(
                    duckdb_re2::StringPiece(pat.GetData(), pat.GetSize()));
                if (!re.ok())
                  return 0;
                duckdb_re2::StringPiece match;
                duckdb_re2::StringPiece input(expr.GetData(), expr.GetSize());
                if (re.Match(input, 0, expr.GetSize(),
                             duckdb_re2::RE2::UNANCHORED, &match, 1))
                  return (int32_t)(match.data() - expr.GetData()) + 1;
                return 0;
              });
        }));
    duckdb::CreateScalarFunctionInfo info(std::move(set));
    info.on_conflict= duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
    catalog.CreateFunction(transaction, info);
  }

  /* regexp_replace(VARCHAR, VARCHAR, VARCHAR) → VARCHAR
     Global (replace-all) MariaDB semantics with bind-time pattern
     compilation for constant patterns.  See regexp_replace_global_func. */
  {
    duckdb::ScalarFunctionSet set("regexp_replace");
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR,
         duckdb::LogicalType::VARCHAR},
        duckdb::LogicalType::VARCHAR, regexp_replace_global_func,
        regexp_replace_bind, nullptr, nullptr, duckdb::RegexInitLocalState));
    duckdb::CreateScalarFunctionInfo info(std::move(set));
    info.on_conflict= duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
    catalog.CreateFunction(transaction, info);
  }

  /* regexp_substr(VARCHAR, VARCHAR) → VARCHAR
     Returns the substring matching pattern, or NULL if no match. */
  {
    duckdb::ScalarFunctionSet set("regexp_substr");
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR},
        duckdb::LogicalType::VARCHAR,
        [](duckdb::DataChunk &args, duckdb::ExpressionState &,
           duckdb::Vector &result) {
          duckdb::BinaryExecutor::ExecuteWithNulls<duckdb::string_t,
                                                   duckdb::string_t,
                                                   duckdb::string_t>(
              args.data[0], args.data[1], result, args.size(),
              [&](duckdb::string_t expr, duckdb::string_t pat,
                  duckdb::ValidityMask &mask,
                  duckdb::idx_t idx) -> duckdb::string_t {
                duckdb_re2::RE2 re(
                    duckdb_re2::StringPiece(pat.GetData(), pat.GetSize()));
                if (!re.ok())
                {
                  mask.SetInvalid(idx);
                  return duckdb::string_t();
                }
                duckdb_re2::StringPiece match;
                duckdb_re2::StringPiece input(expr.GetData(), expr.GetSize());
                if (re.Match(input, 0, expr.GetSize(),
                             duckdb_re2::RE2::UNANCHORED, &match, 1))
                  return duckdb::StringVector::AddString(
                      result, match.data(), match.size());
                mask.SetInvalid(idx);
                return duckdb::string_t();
              });
        }));
    duckdb::CreateScalarFunctionInfo info(std::move(set));
    info.on_conflict= duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
    catalog.CreateFunction(transaction, info);
  }

  /* json_unquote(VARCHAR) → VARCHAR
     Removes JSON quotes and unescapes. Simple implementation. */
  {
    duckdb::ScalarFunctionSet set("json_unquote");
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::VARCHAR}, duckdb::LogicalType::VARCHAR,
        [](duckdb::DataChunk &args, duckdb::ExpressionState &,
           duckdb::Vector &result) {
          duckdb::UnaryExecutor::Execute<duckdb::string_t, duckdb::string_t>(
              args.data[0], result, args.size(),
              [&](duckdb::string_t input) -> duckdb::string_t {
                auto data= input.GetData();
                auto size= input.GetSize();
                /* If not quoted, return as-is */
                if (size < 2 || data[0] != '"' || data[size - 1] != '"')
                  return input;
                /* Strip quotes and unescape */
                std::string out;
                out.reserve(size);
                for (size_t i= 1; i < size - 1; i++)
                {
                  if (data[i] == '\\' && i + 1 < size - 1)
                  {
                    i++;
                    switch (data[i])
                    {
                    case '"':  out+= '"'; break;
                    case '\\': out+= '\\'; break;
                    case '/':  out+= '/'; break;
                    case 'b':  out+= '\b'; break;
                    case 'f':  out+= '\f'; break;
                    case 'n':  out+= '\n'; break;
                    case 'r':  out+= '\r'; break;
                    case 't':  out+= '\t'; break;
                    default:   out+= '\\'; out+= data[i]; break;
                    }
                  }
                  else
                    out+= data[i];
                }
                return duckdb::StringVector::AddString(result, out);
              });
        }));
    duckdb::CreateScalarFunctionInfo info(std::move(set));
    info.on_conflict= duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
    catalog.CreateFunction(transaction, info);
  }

  /* addtime(TIMESTAMP/TIME, VARCHAR) → TIMESTAMP/TIME */
  {
    duckdb::ScalarFunctionSet set("addtime");
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::TIMESTAMP, duckdb::LogicalType::VARCHAR},
        duckdb::LogicalType::TIMESTAMP, addtime_func));
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::TIME, duckdb::LogicalType::VARCHAR},
        duckdb::LogicalType::TIME,
        [](duckdb::DataChunk &args, duckdb::ExpressionState &,
           duckdb::Vector &result) {
          duckdb::BinaryExecutor::Execute<duckdb::dtime_t, duckdb::string_t,
                                          duckdb::dtime_t>(
              args.data[0], args.data[1], result, args.size(),
              [](duckdb::dtime_t t, duckdb::string_t s) -> duckdb::dtime_t {
                int64_t us= parse_mariadb_interval_us(s.GetData(),
                                                      s.GetSize());
                /* Wrap around 24h for DuckDB TIME range */
                int64_t r= t.micros + us;
                const int64_t day_us= 86400LL * 1000000;
                r= ((r % day_us) + day_us) % day_us;
                return duckdb::dtime_t(r);
              });
        }));
    duckdb::CreateScalarFunctionInfo info(std::move(set));
    info.on_conflict= duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
    catalog.CreateFunction(transaction, info);
  }

  /* subtime(TIMESTAMP/TIME, VARCHAR) → TIMESTAMP/TIME */
  {
    duckdb::ScalarFunctionSet set("subtime");
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::TIMESTAMP, duckdb::LogicalType::VARCHAR},
        duckdb::LogicalType::TIMESTAMP, subtime_func));
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::TIME, duckdb::LogicalType::VARCHAR},
        duckdb::LogicalType::TIME,
        [](duckdb::DataChunk &args, duckdb::ExpressionState &,
           duckdb::Vector &result) {
          duckdb::BinaryExecutor::Execute<duckdb::dtime_t, duckdb::string_t,
                                          duckdb::dtime_t>(
              args.data[0], args.data[1], result, args.size(),
              [](duckdb::dtime_t t, duckdb::string_t s) -> duckdb::dtime_t {
                int64_t us= parse_mariadb_interval_us(s.GetData(),
                                                      s.GetSize());
                int64_t r= t.micros - us;
                const int64_t day_us= 86400LL * 1000000;
                r= ((r % day_us) + day_us) % day_us;
                return duckdb::dtime_t(r);
              });
        }));
    duckdb::CreateScalarFunctionInfo info(std::move(set));
    info.on_conflict= duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
    catalog.CreateFunction(transaction, info);
  }

  /* rtrim(VARCHAR, VARCHAR) — substring semantics (MariaDB TRIM) */
  {
    duckdb::ScalarFunctionSet set("rtrim");
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR},
        duckdb::LogicalType::VARCHAR, rtrim_substr_func));
    duckdb::CreateScalarFunctionInfo info(std::move(set));
    info.on_conflict= duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
    catalog.CreateFunction(transaction, info);
  }

  /* ltrim(VARCHAR, VARCHAR) — substring semantics (MariaDB TRIM) */
  {
    duckdb::ScalarFunctionSet set("ltrim");
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR},
        duckdb::LogicalType::VARCHAR, ltrim_substr_func));
    duckdb::CreateScalarFunctionInfo info(std::move(set));
    info.on_conflict= duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
    catalog.CreateFunction(transaction, info);
  }

  /* week(DATE/TIMESTAMP, INTEGER) -> BIGINT (MariaDB mode arg) */
  {
    duckdb::ScalarFunctionSet set("week");
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::DATE, duckdb::LogicalType::INTEGER},
        duckdb::LogicalType::BIGINT, week_2arg_date_func));
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::TIMESTAMP, duckdb::LogicalType::INTEGER},
        duckdb::LogicalType::BIGINT, week_2arg_ts_func));
    duckdb::CreateScalarFunctionInfo info(std::move(set));
    info.on_conflict= duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
    catalog.CreateFunction(transaction, info);
  }

  /* yearweek(DATE/TIMESTAMP, INTEGER) -> BIGINT (MariaDB mode arg) */
  {
    duckdb::ScalarFunctionSet set("yearweek");
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::DATE, duckdb::LogicalType::INTEGER},
        duckdb::LogicalType::BIGINT, yearweek_2arg_date_func));
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::TIMESTAMP, duckdb::LogicalType::INTEGER},
        duckdb::LogicalType::BIGINT, yearweek_2arg_ts_func));
    duckdb::CreateScalarFunctionInfo info(std::move(set));
    info.on_conflict= duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
    catalog.CreateFunction(transaction, info);
  }

  /* to_days(DATE/VARCHAR) -> BIGINT (MariaDB day number since year 0) */
  {
    duckdb::ScalarFunctionSet set("to_days");
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::DATE}, duckdb::LogicalType::BIGINT,
        to_days_date_func));
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::VARCHAR}, duckdb::LogicalType::BIGINT,
        to_days_varchar_func));
    duckdb::CreateScalarFunctionInfo info(std::move(set));
    info.on_conflict= duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
    catalog.CreateFunction(transaction, info);
  }

  /* dayofweek(DATE/TIMESTAMP) -> BIGINT (MariaDB 1=Sunday..7=Saturday) */
  {
    duckdb::ScalarFunctionSet set("dayofweek");
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::DATE}, duckdb::LogicalType::BIGINT,
        dayofweek_date_func));
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::TIMESTAMP}, duckdb::LogicalType::BIGINT,
        dayofweek_ts_func));
    duckdb::CreateScalarFunctionInfo info(std::move(set));
    info.on_conflict= duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
    catalog.CreateFunction(transaction, info);
  }

  /* weekday(DATE/TIMESTAMP) -> BIGINT (MariaDB 0=Monday..6=Sunday) */
  {
    duckdb::ScalarFunctionSet set("weekday");
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::DATE}, duckdb::LogicalType::BIGINT,
        weekday_date_func));
    set.AddFunction(duckdb::ScalarFunction(
        {duckdb::LogicalType::TIMESTAMP}, duckdb::LogicalType::BIGINT,
        weekday_ts_func));
    duckdb::CreateScalarFunctionInfo info(std::move(set));
    info.on_conflict= duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
    catalog.CreateFunction(transaction, info);
  }

  sql_print_information(
      "DuckDB: registered MySQL-compatible function overloads "
      "(octet_length, length, ascii, ord, hex, oct, bin, locate, mid, "
      "rtrim, ltrim, regexp_instr, regexp_replace, regexp_substr, "
      "json_unquote, json_contains, week, yearweek, to_days, dayofweek, "
      "weekday)");
}

} /* namespace myduck */
