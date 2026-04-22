# Capture InnoDB index statistics snapshot.
# Shifts previous $_curr values to $_prev before capturing new values.
# After two calls, $*_prev and $*_curr are set for delta computation.

let $ps_prev= $ps_curr;
let $ma_prev= $ma_curr;
let $ms_prev= $ms_curr;
let $rs_prev= $rs_curr;
let $pd_prev= $pd_curr;
let $lu_prev= $lu_curr;
let $pi_prev= $pi_curr;
let $pu_prev= $pu_curr;
let $pe_prev= $pe_curr;
let $ou_prev= $ou_curr;
let $oo_prev= $oo_curr;
let $ix_prev= $ix_curr;

let $ps_curr= `SELECT COUNT_RESET FROM INFORMATION_SCHEMA.INNODB_METRICS
               WHERE NAME = 'index_page_splits'`;
let $ma_curr= `SELECT COUNT_RESET FROM INFORMATION_SCHEMA.INNODB_METRICS
               WHERE NAME = 'index_page_merge_attempts'`;
let $ms_curr= `SELECT COUNT_RESET FROM INFORMATION_SCHEMA.INNODB_METRICS
               WHERE NAME = 'index_page_merge_successful'`;
let $rs_curr= `SELECT COUNT_RESET FROM INFORMATION_SCHEMA.INNODB_METRICS
               WHERE NAME = 'index_page_reorg_successful'`;
let $pd_curr= `SELECT COUNT_RESET FROM INFORMATION_SCHEMA.INNODB_METRICS
               WHERE NAME = 'index_page_discards'`;
let $lu_curr= `SELECT VARIABLE_VALUE FROM INFORMATION_SCHEMA.GLOBAL_STATUS
               WHERE VARIABLE_NAME = 'Innodb_btr_cur_n_index_lock_upgrades'`;
let $pi_curr= `SELECT VARIABLE_VALUE FROM INFORMATION_SCHEMA.GLOBAL_STATUS
               WHERE VARIABLE_NAME = 'Innodb_btr_cur_pessimistic_insert_calls'`;
let $pu_curr= `SELECT VARIABLE_VALUE FROM INFORMATION_SCHEMA.GLOBAL_STATUS
               WHERE VARIABLE_NAME = 'Innodb_btr_cur_pessimistic_update_calls'`;
let $pe_curr= `SELECT VARIABLE_VALUE FROM INFORMATION_SCHEMA.GLOBAL_STATUS
               WHERE VARIABLE_NAME = 'Innodb_btr_cur_pessimistic_delete_calls'`;
let $ou_curr= `SELECT VARIABLE_VALUE FROM INFORMATION_SCHEMA.GLOBAL_STATUS
               WHERE VARIABLE_NAME = 'Innodb_btr_cur_pessimistic_update_optim_err_underflows'`;
let $oo_curr= `SELECT VARIABLE_VALUE FROM INFORMATION_SCHEMA.GLOBAL_STATUS
               WHERE VARIABLE_NAME = 'Innodb_btr_cur_pessimistic_update_optim_err_overflows'`;
let $ix_curr= `SELECT VARIABLE_VALUE FROM INFORMATION_SCHEMA.GLOBAL_STATUS
               WHERE VARIABLE_NAME = 'Innodb_mtr_n_index_x_lock_calls'`;
