--let $decl_cursor_sample1=/*empty*/
--let $decl_var_sample1=/*empty*/
--let $return=t1%ROWTYPE
--source sp-ref_cursor-fetch-into-row-bad.inc

--let $decl_cursor_sample1=/*empty*/
--let $decl_var_sample1=var_sample1 t1%ROWTYPE;
--let $return=var_sample1%TYPE
--source sp-ref_cursor-fetch-into-row-bad.inc
