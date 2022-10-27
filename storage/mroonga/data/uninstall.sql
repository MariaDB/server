DROP FUNCTION IF EXISTS last_insert_grn_id;
DROP FUNCTION IF EXISTS mroonga_snippet;
DROP FUNCTION IF EXISTS mroonga_command;
DROP FUNCTION IF EXISTS mroonga_escape;
DROP FUNCTION IF EXISTS mroonga_snippet_html;
DROP FUNCTION IF EXISTS mroonga_normalize;
DROP FUNCTION IF EXISTS mroonga_highlight_html;
DROP FUNCTION IF EXISTS mroonga_query_expand;

UNINSTALL PLUGIN Mroonga;

FLUSH TABLES;
