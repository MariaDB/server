
# Tests for PERFORMANCE_SCHEMA

update performance_schema.setup_instruments set enabled='YES';

disconnect con1;
disconnect con2;
disconnect con3;
connection default;

