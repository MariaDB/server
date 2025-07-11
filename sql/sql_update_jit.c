// Code for MIR-JIT compilation.
// Do not include this file directly, compiles via scripts/gen_jit_mir_header.sh

typedef unsigned long long int ha_rows;

int process_triggers_after(void* table, void* thd, void* fields);

int process_triggers_before(void *thd, void *table,
                           void *fields,
                           void *values
                          );

int process_batch_update(void* table, ha_rows* limit,
                         ha_rows* updated, ha_rows* dup_key_found);

int process_update_row(void* table); // TODO: Code will be also jitted inside this func

int process_cut_fields_for_portion_of_time(void* table);

void process_vers_update_end(void* table);

int process_vers_insert_history(void* table, ha_rows* rows_inserted);

int process_check_view_conds(void* table, ha_rows* found, void* ignore);

int period_make_inserts(void* table, void* thd, ha_rows* rows_inserted);

int process_fill_record(void* thd, void* table, void* fields, void* values);

int compare_record(void* table);

process_dec_limit_update(void* table, ha_rows* limit, ha_rows *dup_key_found, ha_rows *updated, const int will_batch);

int update_row_jit(
  void *table,
  void *thd,
  void *fields,
  void *values,
  ha_rows *limit,
  ha_rows *updated,
  ha_rows *dup_key_found,
  ha_rows *rows_inserted,
  ha_rows *found,
  ha_rows *updated_or_same,
  void *ignore,
  int can_compare_record,
  int code_err_record_is_same,
  int will_batch
) {
    int error = 0;

    if (1) { // has_period
        error = process_cut_fields_for_portion_of_time(table);
        if (error) {
            return error;
        }
    }

    if (process_fill_record(thd, table, fields, values)) {
        return 1;
    }


    (*found)++;

    const int need_update= !can_compare_record || compare_record(table);

    if (2) { //is_triggers_before
        error = process_triggers_before(thd, table, fields, values);
        if (error) {
            return error;
        }
    }

    // ^^ Before need_update ^^

    if (need_update) {
        if (3) { //is_versioned
            process_vers_update_end(table);
        }

        if (4) { //is_check_view_conds
            error = process_check_view_conds(table, found, ignore);
            if (error) {
                return error;
            }
        }
        if (5) { //will_batch
            error = process_batch_update(table, limit, updated, dup_key_found);
        } else {
            error = process_update_row(table);
        }

        const int record_was_same= error == code_err_record_is_same;
        if (record_was_same) {
            error= 0;
        } else if (error) {
            return error;
        } else {
            (*updated)++;
        }
        (*updated_or_same)++;

        if (1 && !record_was_same) { // has_period TODO: check how mir will work in this situation
            error = period_make_inserts(table, thd, rows_inserted);
            if (error) {
                return error;
            }
        }
    } else // need update end
        (*updated_or_same)++;
    
    if (6) { // is_vers_insert_history
        error = process_vers_insert_history(table, rows_inserted);
        if (error) {
            return error;
        }
    }

    if (7) { // is_triggers_after
        error = process_triggers_after(table, thd, fields);
        if (error) {
            return error;
        }
    }

    

    if (8) { //using_limit
        error = process_dec_limit_update(table, limit, dup_key_found, updated, will_batch);

        if (error) {
            return error;
        }
    }

    return 0;
}
