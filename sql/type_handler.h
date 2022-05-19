//
// Created by nik on 20.05.22.
//
#include "mariadb.h"
#include "sql_type.h"
#ifndef MYSQL_TYPE_HANDLER_H
#define MYSQL_TYPE_HANDLER_H

Named_type_handler<Type_handler_row> type_handler_row("row");

Named_type_handler<Type_handler_null> type_handler_null("null");

Named_type_handler<Type_handler_bool> type_handler_bool("boolean");
Named_type_handler<Type_handler_tiny> type_handler_stiny("tinyint");
Named_type_handler<Type_handler_short> type_handler_sshort("smallint");
Named_type_handler<Type_handler_long> type_handler_slong("int");
Named_type_handler<Type_handler_int24> type_handler_sint24("mediumint");
Named_type_handler<Type_handler_longlong> type_handler_slonglong("bigint");
Named_type_handler<Type_handler_utiny> type_handler_utiny("tiny unsigned");
Named_type_handler<Type_handler_ushort> type_handler_ushort("smallint unsigned");
Named_type_handler<Type_handler_ulong> type_handler_ulong("int unsigned");
Named_type_handler<Type_handler_uint24> type_handler_uint24("mediumint unsigned");
Named_type_handler<Type_handler_ulonglong> type_handler_ulonglong("bigint unsigned");
Named_type_handler<Type_handler_vers_trx_id> type_handler_vers_trx_id("bigint unsigned");
Named_type_handler<Type_handler_float> type_handler_float("float");
Named_type_handler<Type_handler_double> type_handler_double("double");
Named_type_handler<Type_handler_bit> type_handler_bit("bit");

Named_type_handler<Type_handler_olddecimal> type_handler_olddecimal("decimal");
Named_type_handler<Type_handler_newdecimal> type_handler_newdecimal("decimal");

Named_type_handler<Type_handler_year> type_handler_year("year");
Named_type_handler<Type_handler_year> type_handler_year2("year");
Named_type_handler<Type_handler_time> type_handler_time("time");
Named_type_handler<Type_handler_date> type_handler_date("date");
Named_type_handler<Type_handler_timestamp> type_handler_timestamp("timestamp");
Named_type_handler<Type_handler_timestamp2> type_handler_timestamp2("timestamp");
Named_type_handler<Type_handler_datetime> type_handler_datetime("datetime");
Named_type_handler<Type_handler_time2> type_handler_time2("time");
Named_type_handler<Type_handler_newdate> type_handler_newdate("date");
Named_type_handler<Type_handler_datetime2> type_handler_datetime2("datetime");

Named_type_handler<Type_handler_enum> type_handler_enum("enum");
Named_type_handler<Type_handler_set> type_handler_set("set");

Named_type_handler<Type_handler_string> type_handler_string("char");
Named_type_handler<Type_handler_var_string> type_handler_var_string("varchar");
Named_type_handler<Type_handler_varchar> type_handler_varchar("varchar");
Named_type_handler<Type_handler_hex_hybrid> type_handler_hex_hybrid("hex_hybrid");
Named_type_handler<Type_handler_varchar_compressed> type_handler_varchar_compressed("varchar");

Named_type_handler<Type_handler_tiny_blob> type_handler_tiny_blob("tinyblob");
Named_type_handler<Type_handler_medium_blob> type_handler_medium_blob("mediumblob");
Named_type_handler<Type_handler_long_blob> type_handler_long_blob("longblob");
Named_type_handler<Type_handler_blob> type_handler_blob("blob");
Named_type_handler<Type_handler_blob_compressed> type_handler_blob_compressed("blob");

#endif //MYSQL_TYPE_HANDLER_H
