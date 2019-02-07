# It's supposed that for strict mode some additional checks must be provided,
# what can lead to necessity to read some data from storage,
# while the general optimization for fast update and upsert is that
# there are no read operations during insert/update execution.
# That is why strict mode must be turned off for fast update/upsert testing.

set session sql_mode=(select replace(@@sql_mode,'STRICT_TRANS_TABLES',''));
set session sql_mode=(select replace(@@sql_mode,'STRICT_ALL_TABLES',''));
