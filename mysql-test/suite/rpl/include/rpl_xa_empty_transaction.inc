#
#   Helper file to run each empty-due-to-err XA transaction test case both with
# and without detaching from the connection when the transaction is prepared.
#

--let $use_disconnect=0
--source rpl_xa_empty_transaction_test_case.inc

--let $use_disconnect=1
--source rpl_xa_empty_transaction_test_case.inc
