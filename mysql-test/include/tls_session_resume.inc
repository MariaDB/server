# Open a TLS connection to the server, save the negotiated session, then
# open a second one offering that session. Echoes RESUMED or FULL, and how
# the server side counters moved.
#
# $tls_opt - extra "openssl s_client" options, e.g. -tls1_3
#
# -ign_eof is needed to get a session at all: a TLS 1.3 server sends its
# tickets after the handshake, and without it s_client is gone before they
# arrive. It then has to be given a reason to stop, hence the packet - one
# byte long, which is not a login packet, so the connection ends there
# instead of s_client waiting for input forever. s_client exits non zero
# for the same reason.

--let $tls_session= $MYSQL_TMP_DIR/tls_session.pem
--let $s_client= openssl s_client -connect 127.0.0.1:$MASTER_MYPORT -starttls mysql -ign_eof $tls_opt
--let $status= SELECT VARIABLE_VALUE FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME=

eval set @hits=($status 'Ssl_session_cache_hits'),
        @stored=($status 'Ssl_used_session_cache_entries');

--echo openssl s_client -ign_eof $tls_opt -sess_out tls_session.pem
--error 1
--exec printf '\001\000\000\001\000' | $s_client -sess_out $tls_session >/dev/null 2>&1

--echo openssl s_client -ign_eof $tls_opt -sess_in tls_session.pem
--exec printf '\001\000\000\001\000' | $s_client -sess_in $tls_session 2>/dev/null | grep -q '^Reused' && echo RESUMED || echo FULL

eval select ($status 'Ssl_session_cache_hits') - @hits as 'hits increase',
            ($status 'Ssl_used_session_cache_entries') - @stored as 'stored on the server';
