diff --git a/storage/innobase/handler/ha_innodb.cc b/storage/innobase/handler/ha_innodb.cc
index 6cbf6774dc7..7233fe6c745 100644
--- a/storage/innobase/handler/ha_innodb.cc
+++ b/storage/innobase/handler/ha_innodb.cc
@@ -108,6 +108,7 @@ MYSQL_PLUGIN_IMPORT extern char mysql_unpacked_real_data_home[];
 #endif /* UNIV_DEBUG */
 #include "fts0priv.h"
 #include "page0zip.h"
+#include "dict0priv.h"
 
 #define thd_get_trx_isolation(X) ((enum_tx_isolation)thd_tx_isolation(X))
 
@@ -8598,7 +8599,8 @@ ha_innobase::delete_row(
             wsrep_on(user_thd)                           &&
             !wsrep_thd_skip_append_keys(user_thd))
         {
-		if (wsrep_append_keys(user_thd, false, record, NULL)) {
+		if (wsrep_append_keys(user_thd, WSREP_KEY_EXCLUSIVE, record,
+			              NULL)) {
 			DBUG_PRINT("wsrep", ("delete fail"));
 			error = (dberr_t)HA_ERR_INTERNAL_ERROR;
 			goto wsrep_error;
