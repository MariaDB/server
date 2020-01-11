# Copyright (C) 2013  Kouhei Sutou <kou@clear-code.com>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Library General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Library General Public License for more details.
#
# You should have received a copy of the GNU Library General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA

EXTRA_DIST =					\
	CMakeLists.txt

AM_CFLAGS =					\
	$(GROONGA_CFLAGS)

AM_LDFLAGS =					\
	-avoid-version				\
	-module					\
	-no-undefined

LIBS =						\
	$(GROONGA_LIBS)

normalizers_plugins_LTLIBRARIES =
normalizers_plugins_LTLIBRARIES += mysql.la

include mysql_sources.am

ensure-mysql-source-dir:
	@if test -z "$(MYSQL_SOURCE_DIR)"; then		\
	  echo "\$$(MYSQL_SOURCE_DIR) is missing";	\
	  exit 1;					\
	fi

UPDATE_TABLES_TARGETS =								\
	update-general-ci-table							\
	update-unicode-ci-table							\
	update-unicode-ci-except-kana-ci-kana-with-voiced-sound-mark-table	\
	update-unicode-520-ci-table						\
	update-unicode-520-ci-except-kana-ci-kana-with-voiced-sound-mark-table

update-tables: $(UPDATE_TABLES_TARGETS)

update-general-ci-table: ensure-mysql-source-dir
	$(RUBY)						\
	  $(top_srcdir)/tool/generate_utf8_table.rb	\
	  $(MYSQL_SOURCE_DIR)/strings/ctype-utf8.c >	\
	  $(srcdir)/mysql_general_ci_table.h

update-unicode-ci-table: ensure-mysql-source-dir
	$(RUBY)						\
	  $(top_srcdir)/tool/generate_uca_table.rb	\
	  $(MYSQL_SOURCE_DIR)/strings/ctype-uca.c >	\
	  $(srcdir)/mysql_unicode_ci_table.h

update-unicode-ci-except-kana-ci-kana-with-voiced-sound-mark-table: ensure-mysql-source-dir
	$(RUBY)							\
	  $(top_srcdir)/tool/generate_uca_table.rb		\
	  --split-small-kana					\
	  --split-kana-with-voiced-sound-mark			\
	  --split-kana-with-semi-voiced-sound-mark		\
	  --suffix _except_kana_ci_kana_with_voiced_sound_mark	\
	  $(MYSQL_SOURCE_DIR)/strings/ctype-uca.c >		\
	  $(srcdir)/mysql_unicode_ci_except_kana_ci_kana_with_voiced_sound_mark_table.h

update-unicode-520-ci-table: ensure-mysql-source-dir
	$(RUBY)						\
	  $(top_srcdir)/tool/generate_uca_table.rb	\
	  --version=520					\
	  $(MYSQL_SOURCE_DIR)/strings/ctype-uca.c >	\
	  $(srcdir)/mysql_unicode_520_ci_table.h

update-unicode-520-ci-except-kana-ci-kana-with-voiced-sound-mark-table: ensure-mysql-source-dir
	$(RUBY)							\
	  $(top_srcdir)/tool/generate_uca_table.rb		\
	  --version=520						\
	  --split-small-kana					\
	  --split-kana-with-voiced-sound-mark			\
	  --split-kana-with-semi-voiced-sound-mark		\
	  --suffix _except_kana_ci_kana_with_voiced_sound_mark	\
	  $(MYSQL_SOURCE_DIR)/strings/ctype-uca.c >		\
	  $(srcdir)/mysql_unicode_520_ci_except_kana_ci_kana_with_voiced_sound_mark_table.h

