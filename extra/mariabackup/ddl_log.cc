#include "ddl_log.h"
#include "common.h"
#include "my_sys.h"
#include "sql_table.h"
#include "backup_copy.h"
#include "xtrabackup.h"
#include <unordered_set>
#include <functional>
#include <memory>
#include <cstddef>

namespace ddl_log {

struct Entry {
	enum Type {
		CREATE,
		ALTER,
		RENAME,
		REPAIR,
		OPTIMIZE,
		DROP,
		TRUNCATE,
		CHANGE_INDEX,
		BULK_INSERT
	};
	Type type;
	std::string date;
	std::string engine;
	bool partitioned;
	std::string db;
	std::string table;
	std::string id;
	std::string new_engine;
	bool new_partitioned;
	std::string new_db;
	std::string new_table;
	std::string new_id;
};

typedef std::vector<std::unique_ptr<Entry>> entries_t;
typedef std::function<bool(std::unique_ptr<Entry>)> store_entry_func_t;

const char *aria_engine_name = "Aria";
static const char *frm_ext = ".frm";
static const char *database_keyword = "DATABASE";

const std::unordered_map<std::string, std::vector<const char *>> engine_exts =
{
	{"Aria", {".MAD", ".MAI"}},
	{"MyISAM", {".MYD", ".MYI"}},
	{"MRG_MyISAM", {".MRG"}},
	{"ARCHIVE", {".ARM", ".ARZ"}},
	{"CSV", {".CSM", ".CSV"}}
};

static inline bool known_engine(const std::string &engine) {
	return engine_exts.count(engine);
}

// TODO: add error messages
size_t parse(const uchar *buf, size_t buf_size, bool &error_flag,
	store_entry_func_t &store_entry_func) {
	DBUG_ASSERT(buf);
	static constexpr char token_delimiter = '\t';
	static constexpr char line_delimiter = '\n';
	enum {
		TOKEN_FIRST = 0,
		TOKEN_DATE = TOKEN_FIRST,
		TOKEN_TYPE,
		TOKEN_ENGINE,
		TOKEN_PARTITIONED,
		TOKEN_DB,
		TOKEN_TABLE,
		TOKEN_ID,
		TOKEN_MANDATORY = TOKEN_ID,
		TOKEN_NEW_ENGINE,
		TOKEN_NEW_PARTITIONED,
		TOKEN_NEW_DB,
		TOKEN_NEW_TABLE,
		TOKEN_NEW_ID,
		TOKEN_LAST = TOKEN_NEW_ID
	};
	const size_t string_offsets[TOKEN_LAST + 1] = {
		offsetof(Entry, date),
		offsetof(Entry, type), // not a string, be careful
		offsetof(Entry, engine),
		offsetof(Entry, partitioned), // not a string, be careful
		offsetof(Entry, db),
		offsetof(Entry, table),
		offsetof(Entry, id),
		offsetof(Entry, new_engine),
		offsetof(Entry, new_partitioned), // not a string, be careful
		offsetof(Entry, new_db),
		offsetof(Entry, new_table),
		offsetof(Entry, new_id)
	};
	const std::unordered_map<std::string, Entry::Type> str_to_type = {
		{"CREATE", Entry::CREATE},
		{"ALTER", Entry::ALTER},
		{"RENAME", Entry::RENAME},
		// TODO: fix to use uppercase-only
		{"repair", Entry::REPAIR},
		{"optimize", Entry::OPTIMIZE},
		{"DROP", Entry::DROP},
		{"TRUNCATE", Entry::TRUNCATE},
		{"CHANGE_INDEX", Entry::CHANGE_INDEX},
		{"BULK_INSERT", Entry::BULK_INSERT}
	};

	const uchar *new_line = buf;
	const uchar *token_start = buf;
	unsigned token_num = TOKEN_FIRST;

	error_flag = false;

	std::unique_ptr<Entry> entry(new Entry());

	for (const uchar *ptr = buf; ptr < buf + buf_size; ++ptr) {

		if (*ptr != token_delimiter && *ptr != line_delimiter)
			continue;

		if (token_start != ptr) {
			std::string token(token_start, ptr);

			if (token_num == TOKEN_TYPE) {
				const auto type_it = str_to_type.find(token);
				if (type_it == str_to_type.end()) {
					error_flag = true;
					goto exit;
				}
				entry->type = type_it->second;
			}
			else if (token_num == TOKEN_PARTITIONED) {
				entry->partitioned = token[0] - '0';
			}
			else if (token_num == TOKEN_NEW_PARTITIONED) {
				entry->new_partitioned = token[0] - '0';
			}
			else if (token_num <= TOKEN_LAST) {
				DBUG_ASSERT(token_num != TOKEN_TYPE);
				DBUG_ASSERT(token_num != TOKEN_PARTITIONED);
				DBUG_ASSERT(token_num != TOKEN_NEW_PARTITIONED);
				reinterpret_cast<std::string *>
					(reinterpret_cast<uchar *>(entry.get()) + string_offsets[token_num])->
					assign(std::move(token));
			}
			else {
				error_flag = true;
				goto exit;
			}
		}
		token_start = ptr + 1;

		if (*ptr == line_delimiter) {
			if (token_num < TOKEN_MANDATORY) {
				error_flag = true;
				goto exit;
			}
			if (!store_entry_func(std::move(entry))) {
				error_flag = true;
				goto exit;
			}
			entry.reset(new Entry());
			token_num = TOKEN_FIRST;
			new_line = ptr + 1;
		} else
			++token_num;
	}

exit:
	return new_line - buf;
}

bool parse(const char *file_path, store_entry_func_t store_entry_func) {
	DBUG_ASSERT(file_path);
	DBUG_ASSERT(store_entry_func);
	File file= -1;
	bool result = true;
	uchar buf[1024];
	size_t bytes_read = 0;
	size_t buf_read_offset = 0;

	if ((file= my_open(file_path, O_RDONLY | O_SHARE | O_NOFOLLOW | O_CLOEXEC,
		MYF(MY_WME))) < 0) {
		msg("DDL log file %s open failed: %d", file_path, my_errno);
		result = false;
		goto exit;
	}

	while((bytes_read = my_read(
		file, &buf[buf_read_offset], sizeof(buf) - buf_read_offset, MY_WME)) > 0) {
		if (bytes_read == size_t(-1)) {
			msg("DDL log file %s read error: %d", file_path, my_errno);
			result = false;
			break;
		}
		bytes_read += buf_read_offset;
		bool parse_error_flag = false;
		size_t bytes_parsed = parse(
			buf, bytes_read, parse_error_flag, store_entry_func);
		if (parse_error_flag) {
			result = false;
			break;
		}
		size_t rest_size = bytes_read - bytes_parsed;
		if (rest_size)
			memcpy(buf, buf + bytes_parsed, rest_size);
		buf_read_offset = rest_size;
	}

exit:
	if (file >= 0)
		my_close(file, MYF(MY_WME));
	return result;
};


static
bool process_database(
	const char *datadir_path,
	ds_ctxt_t *ds,
	const Entry &entry,
	std::unordered_set<std::string> &dropped_databases) {

	if (entry.type == Entry::Type::CREATE ||
		entry.type == Entry::Type::ALTER) {
		std::string opt_file(datadir_path);
		opt_file.append("/").append(entry.db).append("/db.opt");
		if (!ds->copy_file(opt_file.c_str(), opt_file.c_str(), 0, true)) {
			msg("Failed to re-copy %s.", opt_file.c_str());
			return false;
		}
		if (entry.type == Entry::Type::CREATE)
			dropped_databases.erase(entry.db);
		return true;
	}

	DBUG_ASSERT(entry.type == Entry::Type::DROP);

	std::string db_path(datadir_path);
	db_path.append("/").append(entry.db);
	const char	*dst_path = convert_dst(db_path.c_str());
	if (!ds_remove(ds, dst_path)) {
		dropped_databases.insert(entry.db);
		return true;
	}
	return false;
}

static
std::unique_ptr<std::vector<std::string>>
	find_table_files(
		const char *dir_path,
		const std::string &db,
		const std::string &table) {

	std::unique_ptr<std::vector<std::string>>
		result(new std::vector<std::string>());

	std::string prefix = convert_tablename_to_filepath(dir_path, db, table);
	foreach_file_in_db_dirs(dir_path, [&](const char *file_name)->bool {
		if (!strncmp(file_name, prefix.c_str(), prefix.size())) {
			DBUG_ASSERT(strlen(file_name) >= prefix.size());
			if (file_name[prefix.size()] == '.' ||
				!strncmp(file_name + prefix.size(), "#P#", strlen("#P#")))
				result->push_back(std::string(file_name));
		}
		return true;
	});

	return result;
}

static
bool process_remove(
	const char *datadir_path,
	ds_ctxt_t *ds,
	const Entry &entry,
	bool remove_frm) {

	if (check_if_skip_table(
		std::string(entry.db).append("/").append(entry.table).c_str()))
		return true;

	auto ext_it = engine_exts.find(entry.engine);
	if (ext_it == engine_exts.end())
		return true;

	std::string file_preffix = convert_tablename_to_filepath(datadir_path,
		entry.db, entry.table);
	const char *dst_preffix = convert_dst(file_preffix.c_str());

	for (const char *ext : ext_it->second) {
		std::string old_name(dst_preffix);
		if (!entry.partitioned)
			old_name.append(ext);
		else
			old_name.append("#P#*");
		if (ds_remove(ds, old_name.c_str())) {
			msg("Failed to remove %s.", old_name.c_str());
			return false;
		}
	}

	if (remove_frm) {
		std::string old_frm_name(dst_preffix);
		old_frm_name.append(frm_ext);
		if (ds_remove(ds, old_frm_name.c_str())) {
			msg("Failed to remove %s.", old_frm_name.c_str());
			return false;
		}
	}
	return true;

}

static
bool process_recopy(
	const char *datadir_path,
	ds_ctxt_t *ds,
	const Entry &entry,
	const tables_t &tables) {

	if (check_if_skip_table(
		std::string(entry.db).append("/").append(entry.table).c_str()))
		return true;

	const std::string &new_table_id =
		entry.new_id.empty() ? entry.id : entry.new_id;
	DBUG_ASSERT(!new_table_id.empty());
	const std::string &new_table =
		entry.new_table.empty() ? entry.table : entry.new_table;
	DBUG_ASSERT(!new_table.empty());
	const std::string &new_db =
		entry.new_db.empty() ? entry.db : entry.new_db;
	DBUG_ASSERT(!new_db.empty());
	const std::string &new_engine =
		entry.new_engine.empty() ? entry.engine : entry.new_engine;
	DBUG_ASSERT(!new_engine.empty());

	if (entry.type != Entry::Type::BULK_INSERT) {
		auto table_it = tables.find(table_key(new_db, new_table));
		if (table_it != tables.end() &&
			table_it->second == new_table_id)
			return true;
	}

	if (!entry.new_engine.empty() &&
		entry.engine != entry.new_engine &&
		!known_engine(entry.new_engine)) {
		return process_remove(datadir_path, ds, entry, false);
	}

	if ((entry.partitioned || entry.new_partitioned) &&
		!process_remove(datadir_path, ds, entry, false))
		return false;

	if (entry.partitioned || entry.new_partitioned) {
		auto files = find_table_files(datadir_path, new_db, new_table);
		if (!files.get())
			return true;
		for (const auto &file : *files) {
			const char *dst_path = convert_dst(file.c_str());
			if (!ds->copy_file(file.c_str(), dst_path, 0, true)) {
				msg("Failed to re-copy %s.", file.c_str());
				return false;
			}
		}
		return true;
	}

	auto ext_it = engine_exts.find(new_engine);
	if (ext_it == engine_exts.end())
		return false;

	for (const char *ext : ext_it->second) {
		std::string file_name =
			convert_tablename_to_filepath(datadir_path, new_db, new_table).
				append(ext);
		const char *dst_path = convert_dst(file_name.c_str());
		if (file_exists(file_name.c_str()) &&
			!ds->copy_file(file_name.c_str(), dst_path, 0, true)) {
			msg("Failed to re-copy %s.", file_name.c_str());
			return false;
		}
	}

	std::string frm_file =
		convert_tablename_to_filepath(datadir_path, new_db, new_table).
			append(frm_ext);
	const char *frm_dst_path = convert_dst(frm_file.c_str());
	if (file_exists(frm_file.c_str()) &&
		!ds->copy_file(frm_file.c_str(), frm_dst_path, 0, true)) {
		msg("Failed to re-copy %s.", frm_file.c_str());
		return false;
	}

	return true;
}

static
bool process_rename(
	const char *datadir_path,
	ds_ctxt_t *ds,
	const Entry &entry) {

	if (check_if_skip_table(
		std::string(entry.db).append("/").append(entry.table).c_str()))
		return true;

	DBUG_ASSERT(entry.db != "partition");

	auto ext_it = engine_exts.find(entry.engine);
	if (ext_it == engine_exts.end())
		return false;

	std::string new_preffix = convert_tablename_to_filepath(datadir_path,
		entry.new_db, entry.new_table);
	const char	*dst_path = convert_dst(new_preffix.c_str());

	std::string old_preffix = convert_tablename_to_filepath(datadir_path,
		entry.db, entry.table);
	const char	*src_path = convert_dst(old_preffix.c_str());

	for (const char *ext : ext_it->second) {
		std::string old_name(src_path);
		old_name.append(ext);
		std::string new_name(dst_path);
		new_name.append(ext);
		if (ds_rename(ds, old_name.c_str(), new_name.c_str())) {
			msg("Failed to rename %s to %s.",
				old_name.c_str(), new_name.c_str());
			return false;
		}
	}

	std::string new_frm_file = new_preffix + frm_ext;
	const char	*new_frm_dst = convert_dst(new_frm_file.c_str());
	if (file_exists(new_frm_file.c_str()) &&
		!ds->copy_file(new_frm_file.c_str(), new_frm_dst, 0, true)) {
		msg("Failed to re-copy %s.", new_frm_file.c_str());
		return false;
	}

// TODO: return this code if .frm is copied not under BLOCK_DDL
/*
	std::string old_frm_name(src_path);
	old_frm_name.append(frm_ext);
	std::string new_frm_name(dst_path);
	new_frm_name.append(frm_ext);
	if (ds_rename(ds, old_frm_name.c_str(), new_frm_name.c_str())) {
		msg("Failed to rename %s to %s.",
			old_frm_name.c_str(), new_frm_name.c_str());
		return false;
	}
*/
	return true;
}

bool backup(
	const char *datadir_path,
	ds_ctxt_t *ds,
	const tables_t &tables) {
	DBUG_ASSERT(datadir_path);
	DBUG_ASSERT(ds);
	char ddl_log_path[FN_REFLEN];
	fn_format(ddl_log_path, "ddl", datadir_path, ".log", 0);
	std::vector<std::unique_ptr<Entry>> entries;

	std::unordered_set<std::string> processed_tables;
	std::unordered_set<std::string> dropped_databases;

	bool parsing_result =
		parse(ddl_log_path, [&](std::unique_ptr<Entry> entry)->bool {

			if (entry->engine == database_keyword)
				return process_database(datadir_path, ds, *entry, dropped_databases);

			if (!known_engine(entry->engine) && !known_engine(entry->new_engine))
				return true;

			if (entry->type == Entry::Type::CREATE ||
					(entry->type == Entry::Type::ALTER &&
					!entry->new_engine.empty() &&
					entry->engine != entry->new_engine)) {
				if (!process_recopy(datadir_path, ds, *entry, tables))
					return false;
				processed_tables.insert(table_key(entry->db, entry->table));
				if (entry->type == Entry::Type::ALTER)
					processed_tables.insert(table_key(entry->new_db, entry->new_table));
				return true;
			}

			if (entry->type == Entry::Type::DROP) {
				if (!process_remove(datadir_path, ds, *entry, true))
					return false;
				processed_tables.insert(table_key(entry->db, entry->table));
				return true;
			}
			if (entry->type == Entry::Type::RENAME) {
				if (entry->partitioned) {
					if (!process_remove(datadir_path, ds, *entry, true))
						return false;
					Entry recopy_entry {
						entry->type,
						{},
						entry->new_engine.empty() ? entry->engine : entry->new_engine,
						true,
						entry->new_db,
						entry->new_table,
						entry->new_id,
						{}, true, {}, {}, {}
					};
					if (!process_recopy(datadir_path, ds, recopy_entry, tables))
						return false;
				}
				else if (!process_rename(datadir_path, ds, *entry))
					return false;
				processed_tables.insert(table_key(entry->db, entry->table));
				processed_tables.insert(table_key(entry->new_db, entry->new_table));
				return true;
			}

			entries.push_back(std::move(entry));
			return true;

		});

	if (!parsing_result)
		return false;


	while (!entries.empty()) {
		auto entry = std::move(entries.back());
		entries.pop_back();
		auto tk = table_key(
			entry->new_db.empty() ? entry->db : entry->new_db,
			entry->new_table.empty() ? entry->table : entry->new_table);
		if (dropped_databases.count(entry->db) ||
			dropped_databases.count(entry->new_db))
			continue;
		if (processed_tables.count(tk))
			continue;
		processed_tables.insert(std::move(tk));
		if (!process_recopy(datadir_path, ds, *entry, tables))
			return false;
	}

	return true;
}

} // namespace ddl_log
