if ($stopword_stage == create_table)
{
call mtr.add_suppression("\\[ERROR\\] InnoDB: user stopword table not_defined does not exist.");
call mtr.add_suppression("\\[ERROR\\] InnoDB: user stopword table test/user_stopword_session does not exist.");

select * from information_schema.innodb_ft_default_stopword;

# Create FTS table
eval CREATE TABLE $stopword_table (
        id INT UNSIGNED AUTO_INCREMENT NOT NULL PRIMARY KEY,
        title VARCHAR(200),
        body TEXT,
        FULLTEXT (title,body)
        )$create_options ENGINE=InnoDB;

# Insert six rows
eval INSERT INTO $stopword_table (title,body) VALUES
        ('MySQL Tutorial','DBMS stands for DataBase ...')  ,
        ('How To Use MySQL Well','After you went through a ...'),
        ('Optimizing MySQL','In this tutorial we will show ...'),
        ('1001 MySQL Tricks','1. Never run mysqld as root. 2. ...'),
        ('MySQL vs. YourSQL','In the following database comparison ...'),
        ('MySQL Security','When configured properly, MySQL ...');

# "the" is in the default stopword, it would not be selected
eval SELECT * FROM $stopword_table WHERE MATCH (title,body)
        AGAINST ('the' IN NATURAL LANGUAGE MODE);

# Provide user defined stopword table, if not (correctly) defined,
# it will be rejected
--error ER_WRONG_VALUE_FOR_VAR
set global innodb_ft_server_stopword_table = "not_defined";
set global innodb_ft_server_stopword_table = NULL;

# Define a correct formatted user stopword table
eval create table user_stopword(value varchar(30))$create_options engine = innodb;

# The set operation should be successful
set global innodb_ft_server_stopword_table = "test/user_stopword";

eval drop index title on $stopword_table;

eval create fulltext index idx on $stopword_table(title, body);
}

if ($stopword_stage == select_1)
{
--error 0, ER_INDEX_CORRUPT
eval SELECT * FROM $stopword_table WHERE MATCH (title,body)
        AGAINST ('the' IN NATURAL LANGUAGE MODE);
--error 0, ER_INDEX_CORRUPT
eval SELECT * FROM $stopword_table WHERE MATCH (title,body)
        AGAINST ('this' IN NATURAL LANGUAGE MODE);

}
