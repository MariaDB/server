if [ -e "$MYSQL_TMP_DIR"/fifo ]; then
    rm "$MYSQL_TMP_DIR"/fifo
fi
if [ -e "$MYSQL_TMP_DIR"/done.txt ]; then
    rm "$MYSQL_TMP_DIR"/done.txt
fi
mkfifo "$MYSQL_TMP_DIR"/fifo
cat "$MYSQL_TMP_DIR"/fifo > "$MYSQL_TMP_DIR"/fifo_data.txt && touch "$MYSQL_TMP_DIR"/done.txt &
