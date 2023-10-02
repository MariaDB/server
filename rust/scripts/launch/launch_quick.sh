# Launch without going through cmake install

echo "1;a7addd9adea9978fda19f21e6be987880e68ac92632ca052e5bb42b1a506939a" > /file-keys.txt

cat << EOF > /usr/local/bin/start-server
#!/bin/sh

mariadbd --user=root \
    --plugin-maturity=experimental \
    --log-bin=on \
    --encrypt-binlog=on \
    --innodb-encrypt-log=on \
    --plugin-load-add='file_key_management_chacha=encryption_chacha' \
    --loose-file-key-management-filename=/file-keys.txt \
    --loose-file-key-management-chacha-filename=/file-keys.txt
EOF

cat << EOF > /usr/local/bin/kill-server
#!/bin/sh

pkill mariadbd
EOF

chmod +x /usr/local/bin/start-server
chmod +x /usr/local/bin/kill-server
