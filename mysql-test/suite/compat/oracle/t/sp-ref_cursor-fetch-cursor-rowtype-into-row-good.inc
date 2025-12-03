--let $decl_cursor_sample1=CURSOR cursor_sample1 IS SELECT * FROM t1;
--let $decl_var_sample1=/*empty*/
--let $return=cursor_sample1%ROWTYPE
--source sp-ref_cursor-fetch-into-row-good.inc


--let $decl_cursor_sample1=CURSOR cursor_sample1 IS SELECT * FROM t1;
--let $decl_var_sample1=var_sample1 cursor_sample1%ROWTYPE;
--let $return=var_sample1%TYPE
--source sp-ref_cursor-fetch-into-row-good.inc
