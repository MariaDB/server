--source include/have_innodb.inc

eval CREATE TABLE tpk
(id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,name VARCHAR(100) NOT NULL)
ENGINE=$engine;

--echo #
--echo # Same name `sid` for check and foreign key constraint while creating the table
--echo #
--error ER_DUP_CONSTRAINT_NAME
eval CREATE TABLE tfk
(id INT, c1 INT, c2 INT NOT NULL,
 CONSTRAINT sid FOREIGN KEY (`c1`) REFERENCES tpk (`id`)
 ON DELETE NO ACTION ON UPDATE NO ACTION, CONSTRAINT sid check (c2>15))
 ENGINE=$engine;

# Create table with unique foreign key
eval CREATE TABLE tfk (c1 INT, c2 INT, CONSTRAINT sid FOREIGN KEY (c1)
                       REFERENCES tpk (id)) ENGINE=$engine;
SHOW CREATE TABLE tfk;

--echo #
--echo # Add check constraint with the same name `sid` as foreign key
--echo #
--error ER_DUP_CONSTRAINT_NAME
ALTER TABLE tfk ADD CONSTRAINT Sid CHECK (c2>15);
SHOW CREATE TABLE tfk;

--echo #
--echo # Add new foreign constraint with the same name `sid` as foreign key
--echo #
--error ER_DUP_CONSTRAINT_NAME
ALTER TABLE tfk ADD CONSTRAINT Sid CHECK (c2>20);

# This doesn't drop index KEY('sid')
ALTER TABLE tfk DROP FOREIGN KEY sid;
SHOW CREATE TABLE tfk;

--echo #
--echo # Drop the foreign key will not drop the index so adding new constraint
--echo # will raise an error
--echo #
--error ER_DUP_CONSTRAINT_NAME
ALTER TABLE tfk ADD CONSTRAINT Sid CHECK (c2>15);

# Drop index
ALTER TABLE tfk DROP INDEX sid;
SHOW CREATE TABLE tfk;

# Now we can add check constraint
ALTER TABLE tfk ADD CONSTRAINT Sid CHECK (c2>15);
SHOW CREATE TABLE tfk;
SELECT constraint_name, check_clause FROM information_schema.check_constraints;

--echo #
--echo # Add foreign key with the same name `sid` as check constraint
--echo #
--error ER_DUP_CONSTRAINT_NAME
ALTER TABLE tfk ADD CONSTRAINT sid FOREIGN KEY (c1) REFERENCES tpk (id);
SHOW CREATE TABLE tfk;
SELECT constraint_name, check_clause FROM information_schema.check_constraints;

DROP TABLE tfk;
--echo #
--echo # Create table with unnamed table check constraint and foreign constraint with the same name
--echo #
--error ER_DUP_CONSTRAINT_NAME
eval CREATE TABLE tfk (c1 INT, c2 INT, CONSTRAINT CONSTRAINT_1 FOREIGN KEY (c1)                        REFERENCES tpk (id), CONSTRAINT CHECK (c2>0))
                       ENGINE=$engine;

--echo #
--echo # Field check constraints name same as name of field `c2` and as foreign key
--echo #
# Should we handle this case ?
eval CREATE TABLE tfk (c1 INT, c2 INT check (c2>0), CONSTRAINT c2
                       FOREIGN KEY (c1) REFERENCES tpk (id)) ENGINE=$engine;
SHOW CREATE TABLE tfk;
SELECT constraint_name, check_clause FROM information_schema.check_constraints;

DROP TABLE tfk;
DROP TABLE tpk;
