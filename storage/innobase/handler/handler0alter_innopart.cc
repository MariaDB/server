/* JAN: TODO: MySQL 5.7 InnoDB partitioning. */

/** Prepare inplace alter table.
Allows InnoDB to update internal structures with concurrent
writes blocked (provided that check_if_supported_inplace_alter()
did not return HA_ALTER_INPLACE_NO_LOCK).
This will be invoked before inplace_alter_table().
@param[in]	altered_table	TABLE object for new version of table.
@param[in]	ha_alter_info	Structure describing changes to be done
by ALTER TABLE and holding data used during in-place alter.
@retval true Failure.
@retval false Success. */
bool
ha_innopart::prepare_inplace_alter_table(
	TABLE*			altered_table,
	Alter_inplace_info*	ha_alter_info)
{
	THD* thd;
	ha_innopart_inplace_ctx* ctx_parts;
	bool res = true;
	DBUG_ENTER("ha_innopart::prepare_inplace_alter_table");
	DBUG_ASSERT(ha_alter_info->handler_ctx == NULL);

	thd = ha_thd();

	/* Clean up all ins/upd nodes. */
	clear_ins_upd_nodes();
	/* Based on Sql_alloc class, return NULL for new on failure. */
	ctx_parts = new ha_innopart_inplace_ctx(thd, m_tot_parts);
	if (!ctx_parts) {
		DBUG_RETURN(HA_ALTER_ERROR);
	}

	uint ctx_array_size = sizeof(inplace_alter_handler_ctx*)
				* (m_tot_parts + 1);
	ctx_parts->ctx_array =
		static_cast<inplace_alter_handler_ctx**>(
					ut_malloc(ctx_array_size,
					mem_key_partitioning));
	if (!ctx_parts->ctx_array) {
		DBUG_RETURN(HA_ALTER_ERROR);
	}

	/* Set all to NULL, including the terminating one. */
	memset(ctx_parts->ctx_array, 0, ctx_array_size);

	ctx_parts->prebuilt_array = static_cast<row_prebuilt_t**>(
					ut_malloc(sizeof(row_prebuilt_t*)
							* m_tot_parts,
					mem_key_partitioning));
	if (!ctx_parts->prebuilt_array) {
		DBUG_RETURN(HA_ALTER_ERROR);
	}
	/* For the first partition use the current prebuilt. */
	ctx_parts->prebuilt_array[0] = m_prebuilt;
	/* Create new prebuilt for the rest of the partitions.
	It is needed for the current implementation of
	ha_innobase::commit_inplace_alter_table(). */
	for (uint i = 1; i < m_tot_parts; i++) {
		row_prebuilt_t* tmp_prebuilt;
		tmp_prebuilt = row_create_prebuilt(
					m_part_share->get_table_part(i),
					table_share->reclength);
		/* Use same trx as original prebuilt. */
		tmp_prebuilt->trx = m_prebuilt->trx;
		ctx_parts->prebuilt_array[i] = tmp_prebuilt;
	}

	for (uint i = 0; i < m_tot_parts; i++) {
		m_prebuilt = ctx_parts->prebuilt_array[i];
		m_prebuilt_ptr = ctx_parts->prebuilt_array + i;
		ha_alter_info->handler_ctx = ctx_parts->ctx_array[i];
		set_partition(i);
		res = ha_innobase::prepare_inplace_alter_table(altered_table,
							ha_alter_info);
		update_partition(i);
		ctx_parts->ctx_array[i] = ha_alter_info->handler_ctx;
		if (res) {
			break;
		}
	}
	m_prebuilt = ctx_parts->prebuilt_array[0];
	m_prebuilt_ptr = &m_prebuilt;
	ha_alter_info->handler_ctx = ctx_parts;
	ha_alter_info->group_commit_ctx = ctx_parts->ctx_array;
	DBUG_RETURN(res);
}

/** Inplace alter table.
Alter the table structure in-place with operations
specified using Alter_inplace_info.
The level of concurrency allowed during this operation depends
on the return value from check_if_supported_inplace_alter().
@param[in]	altered_table	TABLE object for new version of table.
@param[in]	ha_alter_info	Structure describing changes to be done
by ALTER TABLE and holding data used during in-place alter.
@retval true Failure.
@retval false Success. */
bool
ha_innopart::inplace_alter_table(
	TABLE*			altered_table,
	Alter_inplace_info*	ha_alter_info)
{
	bool res = true;
	ha_innopart_inplace_ctx* ctx_parts;

	ctx_parts = static_cast<ha_innopart_inplace_ctx*>(
					ha_alter_info->handler_ctx);
	for (uint i = 0; i < m_tot_parts; i++) {
		m_prebuilt = ctx_parts->prebuilt_array[i];
		ha_alter_info->handler_ctx = ctx_parts->ctx_array[i];
		set_partition(i);
		res = ha_innobase::inplace_alter_table(altered_table,
						ha_alter_info);
		ut_ad(ctx_parts->ctx_array[i] == ha_alter_info->handler_ctx);
		ctx_parts->ctx_array[i] = ha_alter_info->handler_ctx;
		if (res) {
			break;
		}
	}
	m_prebuilt = ctx_parts->prebuilt_array[0];
	ha_alter_info->handler_ctx = ctx_parts;
	return(res);
}

/** Commit or rollback inplace alter table.
Commit or rollback the changes made during
prepare_inplace_alter_table() and inplace_alter_table() inside
the storage engine. Note that the allowed level of concurrency
during this operation will be the same as for
inplace_alter_table() and thus might be higher than during
prepare_inplace_alter_table(). (E.g concurrent writes were
blocked during prepare, but might not be during commit).
@param[in]	altered_table	TABLE object for new version of table.
@param[in]	ha_alter_info	Structure describing changes to be done
by ALTER TABLE and holding data used during in-place alter.
@param[in]	commit		true => Commit, false => Rollback.
@retval true Failure.
@retval false Success. */
bool
ha_innopart::commit_inplace_alter_table(
	TABLE*			altered_table,
	Alter_inplace_info*	ha_alter_info,
	bool			commit)
{
	bool res = false;
	ha_innopart_inplace_ctx* ctx_parts;

	ctx_parts = static_cast<ha_innopart_inplace_ctx*>(
					ha_alter_info->handler_ctx);
	ut_ad(ctx_parts);
	ut_ad(ctx_parts->prebuilt_array);
	ut_ad(ctx_parts->prebuilt_array[0] == m_prebuilt);
	if (commit) {
		/* Commit is done through first partition (group commit). */
		ut_ad(ha_alter_info->group_commit_ctx == ctx_parts->ctx_array);
		ha_alter_info->handler_ctx = ctx_parts->ctx_array[0];
		set_partition(0);
		res = ha_innobase::commit_inplace_alter_table(altered_table,
							ha_alter_info,
							commit);
		ut_ad(res || !ha_alter_info->group_commit_ctx);
		goto end;
	}
	/* Rollback is done for each partition. */
	for (uint i = 0; i < m_tot_parts; i++) {
		m_prebuilt = ctx_parts->prebuilt_array[i];
		ha_alter_info->handler_ctx = ctx_parts->ctx_array[i];
		set_partition(i);
		if (ha_innobase::commit_inplace_alter_table(altered_table,
						ha_alter_info, commit)) {
			res = true;
		}
		ut_ad(ctx_parts->ctx_array[i] == ha_alter_info->handler_ctx);
		ctx_parts->ctx_array[i] = ha_alter_info->handler_ctx;
	}
end:
	/* Move the ownership of the new tables back to
	the m_part_share. */
	ha_innobase_inplace_ctx*	ctx;
	for (uint i = 0; i < m_tot_parts; i++) {
		/* TODO: Fix to only use one prebuilt (i.e. make inplace
		alter partition aware instead of using multiple prebuilt
		copies... */
		ctx = static_cast<ha_innobase_inplace_ctx*>(
					ctx_parts->ctx_array[i]);
		if (ctx) {
			m_part_share->set_table_part(i, ctx->prebuilt->table);
			ctx->prebuilt->table = NULL;
			ctx_parts->prebuilt_array[i] = ctx->prebuilt;
		}
	}
	/* The above juggling of prebuilt must be reset here. */
	m_prebuilt = ctx_parts->prebuilt_array[0];
	m_prebuilt->table = m_part_share->get_table_part(0);
	ha_alter_info->handler_ctx = ctx_parts;
	return(res);
}

/** Notify the storage engine that the table structure (.frm) has
been updated.

ha_partition allows inplace operations that also upgrades the engine
if it supports partitioning natively. So if this is the case then
we will remove the .par file since it is not used with ha_innopart
(we use the internal data dictionary instead). */
void
ha_innopart::notify_table_changed()
{
	char	tmp_par_path[FN_REFLEN + 1];
	strxnmov(tmp_par_path, FN_REFLEN, table->s->normalized_path.str,
		".par", NullS);

	if (my_access(tmp_par_path, W_OK) == 0)
	{
		my_delete(tmp_par_path, MYF(0));
	}
}

/** Check if supported inplace alter table.
@param[in]	altered_table	Altered MySQL table.
@param[in]	ha_alter_info	Information about inplace operations to do.
@return	Lock level, not supported or error */
enum_alter_inplace_result
ha_innopart::check_if_supported_inplace_alter(
	TABLE*			altered_table,
	Alter_inplace_info*	ha_alter_info)
{
	DBUG_ENTER("ha_innopart::check_if_supported_inplace_alter");
	DBUG_ASSERT(ha_alter_info->handler_ctx == NULL);

	/* Not supporting these for partitioned tables yet! */

	/* FK not yet supported. */
	if (ha_alter_info->handler_flags
		& (Alter_inplace_info::ADD_FOREIGN_KEY
			| Alter_inplace_info::DROP_FOREIGN_KEY)) {

		ha_alter_info->unsupported_reason = innobase_get_err_msg(
			ER_FOREIGN_KEY_ON_PARTITIONED);
		DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
	}
	/* FTS not yet supported either. */
	if ((ha_alter_info->handler_flags
		    & Alter_inplace_info::ADD_INDEX)) {

		for (uint i = 0; i < ha_alter_info->index_add_count; i++) {
			const KEY* key =
				&ha_alter_info->key_info_buffer[
					ha_alter_info->index_add_buffer[i]];
			if (key->flags & HA_FULLTEXT) {
				DBUG_ASSERT(!(key->flags & HA_KEYFLAG_MASK
					      & ~(HA_FULLTEXT
						  | HA_PACK_KEY
						  | HA_GENERATED_KEY
						  | HA_BINARY_PACK_KEY)));
				ha_alter_info->unsupported_reason =
					innobase_get_err_msg(
					ER_FULLTEXT_NOT_SUPPORTED_WITH_PARTITIONING);
				DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
			}
		}
	}
	/* We cannot allow INPLACE to change order of KEY partitioning fields! */
	if ((ha_alter_info->handler_flags
	     & Alter_inplace_info::ALTER_STORED_COLUMN_ORDER)
	    && !m_part_info->same_key_column_order(
				&ha_alter_info->alter_info->create_list)) {

		DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
	}

	/* Cannot allow INPLACE for drop and create PRIMARY KEY if partition is
	on Primary Key - PARTITION BY KEY() */
	if ((ha_alter_info->handler_flags
	     & (Alter_inplace_info::ADD_PK_INDEX
		| Alter_inplace_info::DROP_PK_INDEX))) {

		/* Check partition by key(). */
		if ((m_part_info->part_type == HASH_PARTITION)
		    && m_part_info->list_of_part_fields
		    && m_part_info->part_field_list.is_empty()) {

			DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
		}

		/* Check sub-partition by key(). */
		if ((m_part_info->subpart_type == HASH_PARTITION)
		    && m_part_info->list_of_subpart_fields
		    && m_part_info->subpart_field_list.is_empty()) {

			DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
		}
	}

	/* Check for PK and UNIQUE should already be done when creating the
	new table metadata.
	(fix_partition_info/check_primary_key+check_unique_key) */

	set_partition(0);
	enum_alter_inplace_result res =
		ha_innobase::check_if_supported_inplace_alter(altered_table,
							ha_alter_info);

	DBEUG_RETURN(res);
}

