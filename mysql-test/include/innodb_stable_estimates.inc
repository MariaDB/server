#
# Include this file in your .test file if your testcase uses InnoDB tables
# requiring stable query plans, which likely requires that InnoDB produces
# stable estimates for #records in tables.
#
# How it works:
# Unstable InnoDB estimates are caused by InnoDB's background statistics
# collection. When you include this file, MTR will use server options from
# include/innodb_stable_estimates.opt, which disables background statistics
# collection.
# (and no, InnoDB team objects to using this configuration for all MTR tests)
#
