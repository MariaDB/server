# Mask the cost value from any field that looks like
#  "xxx_cost" : double_number
# Print the
#  "xxx_cost" : "REPLACED"
# instead
--replace_regex /(_cost": )[0-9.e-]+/\1"REPLACED"/
#--replace_regex /(_cost": )[0-9.e-]+/"REPLACED"/
#--replace_regex /[0-9]+/BBB/

#--replace_regex /("r_engine_stats":) {[^}]*}/\1 REPLACED/

