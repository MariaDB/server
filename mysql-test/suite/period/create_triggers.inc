create or replace table log_tbl(id int auto_increment primary key, log text) engine=myisam;

create or replace procedure log(s text)
  insert into log_tbl(log) values(s);

if (!$trig_table)
{
  die "No $trig_table specified";
}

if (!$trig_cols)
{
  let $trig_cols= s, e;
}

let $old_trig_args= `select REGEXP_REPLACE('$trig_cols', '([[:word:]]+)',
                                           'old.\\\\\\\\1')`;
let $old_trig_args= `select REPLACE('$old_trig_args', ',', ', ", ", ')`;
let $new_trig_args= `select REGEXP_REPLACE('$trig_cols', '([[:word:]]+)',
                                           'new.\\\\\\\\1')`;
let $new_trig_args= `select REPLACE('$new_trig_args', ',', ', ", ", ')`;

eval create trigger tr1upd_$trig_table before update on $trig_table
  for each row call log(CONCAT('>UPD: ', $old_trig_args, ' -> ', $new_trig_args));
eval create trigger tr2upd_$trig_table after  update on $trig_table
  for each row call log(CONCAT('<UPD: ', $old_trig_args, ' -> ', $new_trig_args));
eval create trigger tr1del_$trig_table before delete on $trig_table
  for each row call log(CONCAT('>DEL: ', $old_trig_args));
eval create trigger tr2del_$trig_table after  delete on $trig_table
  for each row call log(CONCAT('<DEL: ', $old_trig_args));
eval create trigger tr1ins_$trig_table before insert on $trig_table
  for each row call log(CONCAT('>INS: ', $new_trig_args));
eval create trigger tr2ins_$trig_table after  insert on $trig_table
  for each row call log(CONCAT('<INS: ', $new_trig_args));


let trig_cols= 0;
let trig_table= 0;
