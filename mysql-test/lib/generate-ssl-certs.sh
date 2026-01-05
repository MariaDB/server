#!/bin/sh

set -xe

# simply run me from mysql-test/
cd std_data/

# boilerplace for "openssl ca" and /etc/ssl/openssl.cnf
rm -rf demoCA
mkdir demoCA demoCA/newcerts
touch demoCA/index.txt
touch demoCA/index.txt.attr
echo 01 > demoCA/serial
echo 01 > demoCA/crlnumber

# Use rsa:3072 at minimum for all keys to be future compatible with next OpenSSL releases
# See level 3 in https://www.openssl.org/docs/man1.1.0/man3/SSL_CTX_set_security_level.html
# Following industry practice, jump directly to rsa:4096 instead of just rsa:3072.

# CA certificate, self-signed
openssl req -x509 -newkey rsa:4096 -keyout cakey.pem -out cacert.pem -days 7300 -nodes -subj '/CN=cacert/C=FI/ST=Helsinki/L=Helsinki/O=MariaDB' -text

# server certificate signing request and private key. Note the very long subject (for MDEV-7859)
openssl req -newkey rsa:4096 -keyout server-key.pem -out demoCA/server-req.pem -days 7300 -nodes -subj '/CN=localhost/C=FI/ST=state or province within country, in other certificates in this file it is the same as L/L=location, usually an address but often ambiguously used/OU=organizational unit name, a division name within an organization/O=organization name, typically a company name'
# convert the key to yassl compatible format
openssl rsa -in server-key.pem -out server-key.pem
# sign the server certificate with CA certificate
openssl ca -keyfile cakey.pem -days 7300 -batch -cert cacert.pem -policy policy_anything -out server-cert.pem -in demoCA/server-req.pem

# server certificate with different validity period (MDEV-16266)
openssl req -newkey rsa:4096 -keyout server-new-key.pem -out demoCA/server-new-req.pem -days 7301 -nodes -subj '/CN=server-new/C=FI/ST=Helsinki/L=Helsinki/O=MariaDB'
openssl rsa -in server-new-key.pem -out server-new-key.pem
openssl ca -keyfile cakey.pem -days 7301 -batch -cert cacert.pem -policy policy_anything -out server-new-cert.pem -in demoCA/server-new-req.pem

# 8K cert
openssl req -newkey rsa:8192 -keyout server8k-key.pem -out demoCA/server8k-req.pem -days 7300 -nodes -subj '/CN=server8k/C=FI/ST=Helsinki/L=Helsinki/O=MariaDB'
openssl rsa -in server8k-key.pem -out server8k-key.pem
openssl ca -keyfile cakey.pem -days 7300 -batch -cert cacert.pem -policy policy_anything -out server8k-cert.pem -in demoCA/server8k-req.pem

# with SubjectAltName, only for OpenSSL 1.0.2+
cat > demoCA/sanext.conf <<EOF
subjectAltName=IP:127.0.0.1, DNS:localhost
EOF
openssl req -newkey rsa:4096 -keyout serversan-key.pem -out demoCA/serversan-req.pem -days 7300 -nodes -subj '/CN=server/C=FI/ST=Helsinki/L=Helsinki/O=MariaDB'
openssl ca -keyfile cakey.pem -extfile demoCA/sanext.conf -days 7300 -batch -cert cacert.pem -policy policy_anything -out serversan-cert.pem -in demoCA/serversan-req.pem

# client cert
openssl req -newkey rsa:4096 -keyout client-key.pem -out demoCA/client-req.pem -days 7300 -nodes -subj '/CN=client/C=FI/ST=Helsinki/L=Helsinki/O=MariaDB'
openssl rsa -in client-key.pem -out client-key.pem
openssl ca -keyfile cakey.pem -days 7300 -batch -cert cacert.pem -policy policy_anything -out client-cert.pem -in demoCA/client-req.pem

# generate combined client cert and key file
cat client-cert.pem client-key.pem > client-certkey.pem

# generate crls
openssl ca -revoke server-cert.pem -keyfile cakey.pem -batch -cert cacert.pem
openssl ca -gencrl -keyfile cakey.pem -crldays 7300 -batch -cert cacert.pem -out server-cert.crl
# we only want to have one certificate per CRL. Un-revoke server-cert.crl
cp demoCA/index.txt.old demoCA/index.txt
openssl ca -revoke client-cert.pem -keyfile cakey.pem -batch -cert cacert.pem
openssl ca -gencrl -keyfile cakey.pem -crldays 7300 -batch -cert cacert.pem -out client-cert.crl

rm -fv crldir/*
cp -v client-cert.crl crldir/`openssl x509 -in client-cert.pem -noout -issuer_hash`.r0

rm -rf demoCA

# --- Certificate Chain ---
# These tests are inspired from the following commit from MySQL Server
# https://github.com/mysql/mysql-server/commit/969afef933f1872c5f38ea93047ef05c4509c335
#
# Credits to salman.s.khan@oracle.com
#
# -------------------------------------------------------------------------------------
#
# STEPS TO GENERATE THE FOLLOWING CHAINED CERTIFICATES WHICH IS USED IN THE TEST CASE :
#
#                       +---------+
#                       | Root CA |
#                       +---------+
#                            |
#               /------------+-----------\
#               |                        |
#      +------------------+     +------------------+
#      | Intermediate CA1 |     | Intermediate CA2 |
#      +------------------+     +------------------+
#               |                        |
#        +-------------+          +-------------+
#        |   Server    |          |   Client    |
#        | certificate |          | certificate |
#        +-------------+          +-------------+

cd cachain

mkdir ca
mkdir server
mkdir clients

mkdir ca/root.certs
touch ca/root.index.txt
touch ca/root.index.txt.attr
echo '01' > ca/root.serial

cat > ca/root.cfg << EOF
[ ca ]
default_ca = CA_default
[ CA_default ]
dir = $PWD/ca
certs = \$dir/certs
database = \$dir/root.index.txt
serial   = \$dir/root.serial
policy= policy_match
[ policy_match ]
organizationName = match
organizationalUnitName = optional
commonName = supplied
emailAddress = optional
[ v3_ca ]
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid:always,issuer
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign
[ v3_ca_intermediate ]
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid:always,issuer
basicConstraints = critical,CA:TRUE,pathlen:0
keyUsage = critical,keyCertSign,cRLSign
EOF

# Generate Root CA key and cert
openssl genrsa -out ca/root.key 4096
openssl req -new -x509 '-sha256' -key ca/root.key -out ca/root.crt -days 7200 -subj "/O=MariaDB/OU=MariaDB/CN=Root CA" -config ca/root.cfg -extensions v3_ca

# Generate Intermediate CA1 key and cert
openssl genrsa -out ca/intermediate_ca1.key 4096
openssl req -new '-sha256' -key ca/intermediate_ca1.key -out ca/intermediate_ca1.csr -subj "/O=MariaDB/OU=MariaDB/CN=Intermediate CA1"

openssl ca -batch -days 7200 -notext -md sha256 -in ca/intermediate_ca1.csr -out ca/intermediate_ca1.crt -keyfile ca/root.key -cert ca/root.crt -outdir ca/root.certs/ -config ca/root.cfg -extensions v3_ca_intermediate

mkdir ca/intermediate_ca1.certs
touch ca/intermediate_ca1.index.txt
touch ca/intermediate_ca1.index.txt.attr
echo '01' > ca/intermediate_ca1.serial

cat > ca/intermediate_ca1.cfg << EOF
[ ca ]
default_ca = CA_default
[ CA_default ]
dir = $PWD/ca
certs = \$dir/intermediate_ca1.certs
database = \$dir/intermediate_ca1.index.txt
serial   = \$dir/intermediate_ca1.serial
policy= policy_match
[ policy_match ]
commonName = supplied
[ alt_names ]
DNS.1 = localhost
IP.1 = 127.0.0.1
[ server_cert ]
basicConstraints = CA:FALSE
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = @alt_names
EOF

# Generate Server key and cert
openssl genrsa -out server/server.key 4096
openssl req -new '-sha256' -key server/server.key -out server/server.csr -subj "/CN=localhost"

openssl ca -batch -days 7200 -notext -md sha256 -in server/server.csr -out server/server.crt -keyfile ca/intermediate_ca1.key -cert ca/intermediate_ca1.crt -outdir ca/intermediate_ca1.certs/ -config ca/intermediate_ca1.cfg -extensions server_cert

# Generate Intermediate CA2 key and cert
openssl genrsa -out ca/intermediate_ca2.key 4096

openssl req -new '-sha256' -key ca/intermediate_ca2.key -out ca/intermediate_ca2.csr -subj "/O=MariaDB/OU=MariaDB/CN=Intermediate CA2"
openssl ca -batch -days 7200 -notext -md sha256 -in ca/intermediate_ca2.csr -out ca/intermediate_ca2.crt -keyfile ca/root.key -cert ca/root.crt -outdir ca/root.certs/ -config ca/root.cfg -extensions v3_ca_intermediate

mkdir ca/intermediate_ca2.certs
touch ca/intermediate_ca2.index.txt
touch ca/intermediate_ca2.index.txt.attr
echo '01' > ca/intermediate_ca2.serial

cat > ca/intermediate_ca2.cfg << EOF
[ ca ]
default_ca = CA_default
[ CA_default ]
dir = $PWD/ca
certs = \$dir/intermediate_ca2.certs
database = \$dir/intermediate_ca2.index.txt
serial   = \$dir/intermediate_ca2.serial
policy= policy_match
[ policy_match ]
commonName = supplied
[ client_cert ]
basicConstraints = CA:FALSE
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = clientAuth
EOF

# Generate Client key and cert
openssl genrsa -out clients/client.key 4096
openssl req -new '-sha256' -key clients/client.key -out clients/client.csr -subj "/CN=client"

openssl ca -batch -days 7200 -notext -md sha256 -in clients/client.csr -out clients/client.crt -keyfile ca/intermediate_ca2.key -cert ca/intermediate_ca2.crt -outdir ca/intermediate_ca2.certs/ -config ca/intermediate_ca2.cfg -extensions client_cert

cat server/server.crt ca/intermediate_ca1.crt > server/server.cachain

cat clients/client.crt ca/intermediate_ca2.crt > clients/client.cachain

cat ca/root.crt ca/intermediate_ca1.crt > ca/root_intermediate_ca1.crt

# Generate Unrelated Root CA key and cert
openssl genrsa -out ca/unrelated_root.key 4096
openssl req -new -x509 '-sha256' -key ca/unrelated_root.key -out ca/unrelated_root.crt -days 7200 -subj "/O=MariaDB/OU=MariaDB/CN=Root CA" -config ca/root.cfg -extensions v3_ca

cp -v ca/root.crt ca/root_intermediate_ca1.crt ca/unrelated_root.crt server/server.key server/server.cachain clients/client.key clients/client.cachain ./
rm -rf ca server clients

cd ..
