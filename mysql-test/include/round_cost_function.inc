delimiter //;
create function round_cost(explain_output text)
returns text
begin
    declare len int default 0;
    declare prec int default 7;
    declare cost_elem text;
    declare cost_value_json text;
    declare cost_value double;

    set cost_elem= '$.query_block[0].cost';
    set cost_value_json= (select json_extract(explain_output, cost_elem));
    set explain_output= (select json_replace(explain_output, cost_elem, round(cost_value_json, prec)));
    set len= (select json_length(json_extract(explain_output, '$.query_block.nested_loop')));

    for i in 0 .. len-1
    do
        set cost_elem= concat('$.query_block[0].nested_loop[', i, '].table.cost');
        set cost_value_json= (select json_extract(explain_output, cost_elem));
        set explain_output= (select json_replace(explain_output, cost_elem, round(cost_value_json, prec)));
        set cost_elem= concat('$.query_block[0].nested_loop[', i, '].block-nl-join.table.cost');
        set cost_value_json= (select json_extract(explain_output, cost_elem));
        set explain_output= (select json_replace(explain_output, cost_elem, round(cost_value_json, prec)));
    end for;
    return explain_output;
end;
//
delimiter ;//
