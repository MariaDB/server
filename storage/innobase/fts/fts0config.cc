/*****************************************************************************

Copyright (c) 2007, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2021, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/******************************************************************//**
@file fts/fts0config.cc
Full Text Search configuration table.

Created 2007/5/9 Sunny Bains
***********************************************************************/

#include "trx0roll.h"
#include "row0sel.h"

#include "fts0exec.h"
#include "fts0priv.h"

/** Get value from the config table. The caller must ensure that enough
space is allocated for value to hold the column contents.
@param trx      transaction
@param table    Indexed fts table
@param name     name of the key
@param value    value of the key
@return DB_SUCCESS or error code */
dberr_t fts_config_get_value(trx_t *trx, const dict_table_t *table,
                             const char *name, fts_string_t *value)
{
  trx->op_info = "getting FTS config value";
  FTSQueryExecutor executor(trx, nullptr, table);
  ConfigReader reader;
  dberr_t err= executor.read_config_with_lock(name, reader);
  if (err == DB_SUCCESS)
  {
    ulint max_len= ut_min(value->f_len - 1, reader.value_span.size());
    memcpy(value->f_str, reader.value_span.data(), max_len);
    value->f_len= max_len;
    value->f_str[value->f_len]= '\0';
    executor.release_lock();
  }
  else value->f_str[0]= '\0';
  return err;
}

/*********************************************************************//**
Create the config table name for retrieving index specific value.
@return index config parameter name */
char*
fts_config_create_index_param_name(
/*===============================*/
	const char*		param,		/*!< in: base name of param */
	const dict_index_t*	index)		/*!< in: index for config */
{
	ulint		len;
	char*		name;

	/* The format of the config name is: name_<index_id>. */
	len = strlen(param);

	/* Caller is responsible for deleting name. */
	name = static_cast<char*>(ut_malloc_nokey(
		len + FTS_AUX_MIN_TABLE_ID_LENGTH + 2));
	::strcpy(name, param);
	name[len] = '_';

	fts_write_object_id(index->id, name + len + 1);

	return(name);
}

/******************************************************************//**
Get value specific to an FTS index from the config table. The caller
must ensure that enough space is allocated for value to hold the
column contents.
@return DB_SUCCESS or error code */
dberr_t
fts_config_get_index_value(
/*=======================*/
	trx_t*		trx,			/*!< transaction */
	dict_index_t*	index,			/*!< in: index */
	const char*	param,			/*!< in: get config value for
						this parameter name */
	fts_string_t*	value)			/*!< out: value read from
						config table */
{
	/* We are responsible for free'ing name. */
	char *name = fts_config_create_index_param_name(param, index);

	dberr_t error = fts_config_get_value(trx, index->table, name, value);

	ut_free(name);

	return(error);
}

/** Set the value in the config table for name.
@param trx       transaction
@param fts_table indexed fulltext table
@param name      key for the config
@param value     value of the key
@return DB_SUCCESS or error code */
dberr_t
fts_config_set_value(trx_t *trx, const dict_table_t *table,
                     const char *name, const fts_string_t *value)
{
  trx->op_info= "setting FTS config value";
  FTSQueryExecutor executor(trx, nullptr, table, table->fts->dict_locked);
  char value_str[FTS_MAX_CONFIG_VALUE_LEN + 1];
  memcpy(value_str, value->f_str, value->f_len);
  value_str[value->f_len]= '\0';
  return executor.update_config_record(name, value_str);
}

/******************************************************************//**
Set the value specific to an FTS index in the config table.
@return DB_SUCCESS or error code */
dberr_t
fts_config_set_index_value(
/*=======================*/
	trx_t*		trx,			/*!< transaction */
	dict_index_t*	index,			/*!< in: index */
	const char*	param,			/*!< in: get config value for
						this parameter name */
	fts_string_t*	value)			/*!< out: value read from
						config table */
{
	/* We are responsible for free'ing name. */
	char *name = fts_config_create_index_param_name(param, index);

	dberr_t error = fts_config_set_value(trx, index->table, name, value);

	ut_free(name);

	return(error);
}

#ifdef FTS_OPTIMIZE_DEBUG
/******************************************************************//**
Get an ulint value from the config table.
@return DB_SUCCESS if all OK else error code */
dberr_t
fts_config_get_index_ulint(
/*=======================*/
	trx_t*		trx,			/*!< in: transaction */
	dict_index_t*	index,			/*!< in: FTS index */
	const char*	name,			/*!< in: param name */
	ulint*		int_value)		/*!< out: value */
{
	dberr_t		error;
	fts_string_t	value;

	/* We set the length of value to the max bytes it can hold. This
	information is used by the callback that reads the value.*/
	value.f_len = FTS_MAX_CONFIG_VALUE_LEN;
	value.f_str = static_cast<byte*>(ut_malloc_nokey(value.f_len + 1));

	error = fts_config_get_index_value(trx, index, name, &value);

	if (UNIV_UNLIKELY(error != DB_SUCCESS)) {
		ib::error() << "(" << error << ") reading `" << name << "'";
	} else {
		*int_value = strtoul((char*) value.f_str, NULL, 10);
	}

	ut_free(value.f_str);

	return(error);
}

/******************************************************************//**
Set an ulint value in the config table.
@return DB_SUCCESS if all OK else error code */
dberr_t
fts_config_set_index_ulint(
/*=======================*/
	trx_t*		trx,			/*!< in: transaction */
	dict_index_t*	index,			/*!< in: FTS index */
	const char*	name,			/*!< in: param name */
	ulint		int_value)		/*!< in: value */
{
	dberr_t		error;
	fts_string_t	value;

	/* We set the length of value to the max bytes it can hold. This
	information is used by the callback that reads the value.*/
	value.f_len = FTS_MAX_CONFIG_VALUE_LEN;
	value.f_str = static_cast<byte*>(ut_malloc_nokey(value.f_len + 1));

	// FIXME: Get rid of snprintf
	ut_a(FTS_MAX_INT_LEN < FTS_MAX_CONFIG_VALUE_LEN);

	value.f_len = snprintf(
		(char*) value.f_str, FTS_MAX_INT_LEN, ULINTPF, int_value);

	error = fts_config_set_index_value(trx, index, name, &value);

	if (UNIV_UNLIKELY(error != DB_SUCCESS)) {
		ib::error() << "(" << error << ") writing `" << name << "'";
	}

	ut_free(value.f_str);

	return(error);
}
#endif /* FTS_OPTIMIZE_DEBUG */

/** Get an ulint value from the config table.
@param trx 	transaction
@param table 	user table
@param name	key value
@param int_value value of the key
@return DB_SUCCESS if all OK else error code */
dberr_t
fts_config_get_ulint(trx_t *trx, const dict_table_t *table,
                     const char *name, ulint *int_value)
{
  fts_string_t	value;
  /* We set the length of value to the max bytes it can hold. This
  information is used by the callback that reads the value.*/
  value.f_len= FTS_MAX_CONFIG_VALUE_LEN;
  value.f_str= static_cast<byte*>(ut_malloc_nokey(value.f_len + 1));
  dberr_t error= fts_config_get_value(trx, table, name, &value);
  if (UNIV_UNLIKELY(error != DB_SUCCESS))
    ib::error() <<  "(" << error << ") reading `" << name << "'";
  else *int_value = strtoul((char*) value.f_str, NULL, 10);
  ut_free(value.f_str);
  return error;
}

/** Set an ulint value in the config table.
@param trx	transaction
@param table	user table
@param name	name of the key
@param int_value value of the key to be set
@return DB_SUCCESS if all OK else error code */
dberr_t
fts_config_set_ulint(trx_t *trx, const dict_table_t *table,
                     const char *name, ulint int_value)
{
  fts_string_t	value;
  /* We set the length of value to the max bytes it can hold. This
  information is used by the callback that reads the value.*/
  value.f_len= FTS_MAX_CONFIG_VALUE_LEN;
  value.f_str= static_cast<byte*>(ut_malloc_nokey(value.f_len + 1));
  ut_a(FTS_MAX_INT_LEN < FTS_MAX_CONFIG_VALUE_LEN);
  value.f_len= (ulint) snprintf((char*) value.f_str, FTS_MAX_INT_LEN,
                                ULINTPF, int_value);
  dberr_t error= fts_config_set_value(trx, table, name, &value);
  if (UNIV_UNLIKELY(error != DB_SUCCESS))
    ib::error() <<  "(" << error << ") writing `" << name << "'";
  ut_free(value.f_str);
  return error;
}
