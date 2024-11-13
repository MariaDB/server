/* Copyright (c) 2024, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA
 */

#include "my_config.h"
#include "import_util.h"

#include <tap.h>
#include <string>

inline bool operator==(const KeyDefinition &lhs, const KeyDefinition &rhs)
{
  return lhs.definition == rhs.definition && lhs.name == rhs.name;
}

/*
  Test parsing of CREATE TABLE in mariadb-import utility
*/
static void test_ddl_parser()
{
  std::string script= R"(
     -- Some SQL script
 CREATE TABLE `book` (
  `id` mediumint(8) unsigned NOT NULL AUTO_INCREMENT,
  `title` varchar(200) NOT NULL,
  `author_id` smallint(5) unsigned NOT NULL,
  `publisher_id` smallint(5) unsigned NOT NULL,
  `excerpt` text,
  PRIMARY KEY (`id`),
  KEY `fk_book_author` (`author_id`),
  KEY `fk_book_publisher` (`publisher_id`),
  UNIQUE KEY `title_author` (`title`,`author`),
  FULLTEXT KEY `excerpt` (`excerpt`),
  CONSTRAINT `fk_book_author` FOREIGN KEY (`author_id`) REFERENCES `author` (`id`) ON DELETE CASCADE
  CONSTRAINT `fk_book_publisher` FOREIGN KEY (`publisher_id`) REFERENCES `publisher` (`id`) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_uca1400_ai_ci;
)";

  auto create_table_stmt= extract_first_create_table(script);
  ok(!create_table_stmt.empty(), "CREATE TABLE statement found");

  TableDDLInfo ddl_info(create_table_stmt);

  const std::string& table_name= ddl_info.table_name;
  const std::string& storage_engine= ddl_info.storage_engine;

  ok(table_name == "`book`", "Table name is OK");
  ok(storage_engine == "InnoDB", "Storage engine is OK");
  ok(ddl_info.primary_key == KeyDefinition{"PRIMARY KEY (`id`)", "PRIMARY"},
     "Primary key def is OK");

  ok(ddl_info.secondary_indexes.size() == 4, "Secondary index size is OK");
  const auto &sec_indexes= ddl_info.secondary_indexes;
  ok(sec_indexes[0] == KeyDefinition{"KEY `fk_book_author` (`author_id`)","`fk_book_author`"},
         "First key is OK");
  ok(sec_indexes[1] ==
         KeyDefinition{"KEY `fk_book_publisher` (`publisher_id`)",
                       "`fk_book_publisher`"},
     "Second key is OK");

  ok(ddl_info.constraints.size() == 2, "Constraints size correct");
  ok(ddl_info.constraints[0] ==
     KeyDefinition{"CONSTRAINT `fk_book_author` FOREIGN KEY (`author_id`) REFERENCES "
     "`author` (`id`) ON DELETE CASCADE","`fk_book_author`"},
     "First constraint OK");

  std::string drop_constraints= ddl_info.drop_constraints_sql();
  ok(drop_constraints ==
     "ALTER TABLE `book` DROP CONSTRAINT `fk_book_author`, DROP CONSTRAINT `fk_book_publisher`",
     "Drop constraints SQL is \"%s\"", drop_constraints.c_str());
  std::string add_constraints= ddl_info.add_constraints_sql();
  ok(add_constraints ==
      "ALTER TABLE `book` ADD CONSTRAINT `fk_book_author` FOREIGN KEY (`author_id`) "
      "REFERENCES `author` (`id`) ON DELETE CASCADE, "
      "ADD CONSTRAINT `fk_book_publisher` FOREIGN KEY (`publisher_id`) "
      "REFERENCES `publisher` (`id`) ON DELETE CASCADE",
    "Add constraints SQL is \"%s\"",add_constraints.c_str());

  std::string drop_secondary_indexes=
      ddl_info.drop_secondary_indexes_sql();
  ok(drop_secondary_indexes ==
     "ALTER TABLE `book` "
      "DROP INDEX `fk_book_author`, "
      "DROP INDEX `fk_book_publisher`, "
      "DROP INDEX `title_author`, "
      "DROP INDEX `excerpt`",
     "Drop secondary indexes SQL is \"%s\"", drop_secondary_indexes.c_str());
  std::string add_secondary_indexes=
      ddl_info.add_secondary_indexes_sql();
  ok(add_secondary_indexes ==
     "ALTER TABLE `book` ADD KEY `fk_book_author` (`author_id`), "
     "ADD KEY `fk_book_publisher` (`publisher_id`), "
     "ADD UNIQUE KEY `title_author` (`title`,`author`), "
     "ADD FULLTEXT KEY `excerpt` (`excerpt`)",
     "Add secondary indexes SQL is \"%s\"", add_secondary_indexes.c_str());
}

/*
 For Innodb table without PK, and but with Unique key
 (which is used for clustering, instead of PK)
 this key will not be added and dropped by
 the import utility
*/
static void innodb_non_pk_clustering_key()
{
  auto create_table_stmt= R"(
  CREATE TABLE `book` (
  `id` mediumint(8),
  `uniq` varchar(200),
   UNIQUE KEY `id` (`id`),
   UNIQUE KEY `uniq` (`uniq`),
   KEY `id_uniq` (`id`,`uniq`)
  ) ENGINE=InnoDB;
 )";
  TableDDLInfo ddl_info(create_table_stmt);
  ok(ddl_info.non_pk_clustering_key_name == "`id`",
     "Non-PK clustering key is %s",
     ddl_info.non_pk_clustering_key_name.c_str());
  ok(ddl_info.primary_key.definition.empty(),
     "Primary key is %s", ddl_info.primary_key.definition.c_str());
  ok(ddl_info.secondary_indexes.size() == 3,
     "Secondary indexes size is %zu",
     ddl_info.secondary_indexes.size());
  ok(!ddl_info.add_secondary_indexes_sql().empty(),
     "Some secondary indexes to add");
  ok(!ddl_info.drop_secondary_indexes_sql().empty(),
     "Some secondary indexes to drop");
}
int main()
{
  plan(18);
  diag("Testing DDL parser");

  test_ddl_parser();
  innodb_non_pk_clustering_key();
  return exit_status();
}
