if ($CLIENT_TLS_LIBRARY != "OpenSSL") {
  if ($CLIENT_TLS_LIBRARY != "LibreSSL") {
    skip "Test requires Connector/C with OpenSSL library";
  }
}
