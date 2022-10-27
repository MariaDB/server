# in the FIPS mode, OpenSSL disables DES and other weak algorithms
source include/have_ssl_crypto_functs.inc;

if (`select des_encrypt("a", "b") IS NULL`) {
  skip DES is disabled (fips mode?);
}
