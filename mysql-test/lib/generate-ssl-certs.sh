#!/bin/sh

set -xe

# simply run me from mysql-test/
cd std_data/

# boilerplace for "openssl ca" and /etc/ssl/openssl.cnf
rm -rf demoCA
mkdir demoCA demoCA/newcerts
touch demoCA/index.txt
echo 01 > demoCA/serial
echo 01 > demoCA/crlnumber

# CA certificate, self-signed
openssl req -x509 -newkey rsa:2048 -keyout cakey.pem -out cacert.pem -days 7300 -nodes -subj '/CN=cacert/C=FI/ST=Helsinki/L=Helsinki/O=MariaDB' -text

# server certificate signing request and private key. Note the very long subject (for MDEV-7859)
openssl req -newkey rsa:2048 -keyout server-key.pem -out demoCA/server-req.pem -days 7300 -nodes -subj '/CN=localhost/C=FI/ST=state or province within country, in other certificates in this file it is the same as L/L=location, usually an address but often ambiguously used/OU=organizational unit name, a division name within an organization/O=organization name, typically a company name'
# convert the key to yassl compatible format
openssl rsa -in server-key.pem -out server-key.pem
# sign the server certificate with CA certificate
openssl ca -keyfile cakey.pem -days 7300 -batch -cert cacert.pem -policy policy_anything -out server-cert.pem -in demoCA/server-req.pem

# server certificate with different validity period (MDEV-7598)
openssl req -newkey rsa:2048 -keyout server-new-key.pem -out demoCA/server-new-req.pem -days 7301 -nodes -subj '/CN=server-new/C=FI/ST=Helsinki/L=Helsinki/O=MariaDB'
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
openssl req -newkey rsa:2048 -keyout serversan-key.pem -out demoCA/serversan-req.pem -days 7300 -nodes -subj '/CN=server/C=FI/ST=Helsinki/L=Helsinki/O=MariaDB'
openssl ca -keyfile cakey.pem -extfile demoCA/sanext.conf -days 7300 -batch -cert cacert.pem -policy policy_anything -out serversan-cert.pem -in demoCA/serversan-req.pem

# client cert
openssl req -newkey rsa:2048 -keyout client-key.pem -out demoCA/client-req.pem -days 7300 -nodes -subj '/CN=client/C=FI/ST=Helsinki/L=Helsinki/O=MariaDB'
openssl rsa -in client-key.pem -out client-key.pem
openssl ca -keyfile cakey.pem -days 7300 -batch -cert cacert.pem -policy policy_anything -out client-cert.pem -in demoCA/client-req.pem

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
