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

#include "fts0priv.h"

/** Get value from the config table. The caller must ensure that enough
space is allocated for value to hold the column contents.
@param trx        transaction
@param fts_table  indexed FTS table
@param name       get config value for this parameter name
@param value      Value read from config table
@return DB_SUCCESS or error code */
dberr_t fts_config_get_value(trx_t *trx, fts_table_t *fts_table,
                             const char *name, fts_string_t *value)
{
  FTSQueryRunner sqlRunner(trx);
  fts_table->suffix = "CONFIG";
  dberr_t err= DB_SUCCESS;
  dict_table_t *table= sqlRunner.open_table(fts_table, &err);
  if (table)
  {
    ut_ad(UT_LIST_GET_LEN(table->indexes) == 1);
    err= sqlRunner.prepare_for_read(table);
    if (err == DB_SUCCESS)
    {
      dict_index_t *clust_index= dict_table_get_first_index(table);
      sqlRunner.build_tuple(clust_index, 1, 1);
      sqlRunner.assign_config_fields(name);

      *value->f_str = '\0';
      ut_a(value->f_len > 0);

      /* Following record executor does the following command
      SELECT value FROM $FTS_PREFIX_CONFIG WHERE key = :name;"
      and stores the value field in fts_string_t value */
      err= sqlRunner.record_executor(clust_index, READ, MATCH_UNIQUE,
                                     PAGE_CUR_GE, &read_fts_config, value);
      /* In case of empty table, error can be DB_END_OF_INDEX
      and key doesn't exist, error can be DB_RECORD_NOT_FOUND */
      if (err == DB_END_OF_INDEX || err == DB_RECORD_NOT_FOUND)
        err= DB_SUCCESS;
    }
    table->release();
  }
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
	char*		name;
	dberr_t		error;
	fts_table_t	fts_table;

	FTS_INIT_FTS_TABLE(&fts_table, "CONFIG", FTS_COMMON_TABLE,
			   index->table);

	/* We are responsible for free'ing name. */
	name = fts_config_create_index_param_name(param, index);

	error = fts_config_get_value(trx, &fts_table, name, value);

	ut_free(name);

	return(error);
}

dberr_t fts_config_set_value(trx_t *trx, fts_table_t *fts_table,
                             const char *name, const fts_string_t *value)
{
  dberr_t err= DB_SUCCESS;
  const bool dict_locked = fts_table->table->fts->dict_locked;
  fts_table->suffix = "CONFIG";

  FTSQueryRunner sqlRunner(trx);
  dict_table_t *fts_config= sqlRunner.open_table(fts_table, &err, dict_locked);
  if (fts_config)
  {
    err= sqlRunner.prepare_for_write(fts_config);
    if (err == DB_SUCCESS)
    {
      ut_ad(UT_LIST_GET_LEN(fts_config->indexes) == 1);
      dict_index_t *clust_index= dict_table_get_first_index(fts_config);
      sqlRunner.build_tuple(clust_index, 1, 1);
      /* We set the length of value to the max bytes it can hold. This
      information is used by the callback that reads the value.*/
      fts_string_t old_value;
      old_value.f_len= FTS_MAX_CONFIG_VALUE_LEN;
      old_value.f_str=
        static_cast<byte*>(ut_malloc_nokey(old_value.f_len + 1));

      /* REPLACE INTO $FTS_PREFIX_CONFIG VALUES (KEY, VALUE) */
      sqlRunner.assign_config_fields(name);
      err= sqlRunner.record_executor(clust_index, SELECT_UPDATE,
                                     MATCH_UNIQUE, PAGE_CUR_GE,
                                     &read_fts_config, &old_value);
      if (err == DB_SUCCESS)
      {
        /* Record already exist. So Update the value field with new value */
        if (old_value.f_len != value->f_len ||
            memcmp(old_value.f_str, value->f_str, value->f_len) != 0)
        {
          /* Build update vector with new value for FTS_CONFIG table */
          sqlRunner.build_update_config(fts_config, 3, value);
          err= sqlRunner.update_record(
            fts_config, static_cast<uint16_t>(old_value.f_len),
            static_cast<uint16_t>(value->f_len));
        }
      }
      else if (err == DB_END_OF_INDEX || err == DB_RECORD_NOT_FOUND)
      {
        /* Record doesn't exist. So do insert the key, value in config table */
        sqlRunner.build_tuple(clust_index);
        sqlRunner.assign_config_fields(name, value->f_str, value->f_len);
        err= sqlRunner.write_record(fts_config);
      }
      ut_free(old_value.f_str);
    }
    fts_config->release();
  }

  return err;
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
	char*		name;
	dberr_t		error;
	fts_table_t	fts_table;

	FTS_INIT_FTS_TABLE(&fts_table, "CONFIG", FTS_COMMON_TABLE,
			   index->table);

	/* We are responsible for free'ing name. */
	name = fts_config_create_index_param_name(param, index);

	error = fts_config_set_value(trx, &fts_table, name, value);

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

/******************************************************************//**
Get an ulint value from the config table.
@return DB_SUCCESS if all OK else error code */
dberr_t
fts_config_get_ulint(
/*=================*/
	trx_t*		trx,			/*!< in: transaction */
	fts_table_t*	fts_table,		/*!< in: the indexed
						FTS table */
	const char*	name,			/*!< in: param name */
	ulint*		int_value)		/*!< out: value */
{
	dberr_t		error;
	fts_string_t	value;

	/* We set the length of value to the max bytes it can hold. This
	information is used by the callback that reads the value.*/
	value.f_len = FTS_MAX_CONFIG_VALUE_LEN;
	value.f_str = static_cast<byte*>(ut_malloc_nokey(value.f_len + 1));

	error = fts_config_get_value(trx, fts_table, name, &value);

	if (UNIV_UNLIKELY(error != DB_SUCCESS)) {
		ib::error() <<  "(" << error << ") reading `" << name << "'";
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
fts_config_set_ulint(
/*=================*/
	trx_t*		trx,			/*!< in: transaction */
	fts_table_t*	fts_table,		/*!< in: the indexed
						FTS table */
	const char*	name,			/*!< in: param name */
	ulint		int_value)		/*!< in: value */
{
	dberr_t		error;
	fts_string_t	value;

	/* We set the length of value to the max bytes it can hold. This
	information is used by the callback that reads the value.*/
	value.f_len = FTS_MAX_CONFIG_VALUE_LEN;
	value.f_str = static_cast<byte*>(ut_malloc_nokey(value.f_len + 1));

	ut_a(FTS_MAX_INT_LEN < FTS_MAX_CONFIG_VALUE_LEN);

	value.f_len = (ulint) snprintf(
		(char*) value.f_str, FTS_MAX_INT_LEN, ULINTPF, int_value);

	error = fts_config_set_value(trx, fts_table, name, &value);

	if (UNIV_UNLIKELY(error != DB_SUCCESS)) {
		ib::error() <<  "(" << error << ") writing `" << name << "'";
	}

	ut_free(value.f_str);

	return(error);
}
