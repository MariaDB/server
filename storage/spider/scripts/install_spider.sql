# Copyright (C) 2010-2019 Kentoku Shiba
# 
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA

drop procedure if exists mysql.spider_plugin_installer;
delimiter //
create procedure mysql.spider_plugin_installer()
begin
  set @win_plugin := IF(@@version_compile_os like 'Win%', 1, 0);
  set @have_spider_i_s_plugin := 0;
  select @have_spider_i_s_plugin := 1 from INFORMATION_SCHEMA.plugins where PLUGIN_NAME = 'SPIDER';
  set @have_spider_plugin := 0;
  select @have_spider_plugin := 1 from mysql.plugin where name = 'spider';
  if @have_spider_i_s_plugin = 0 then
    if @have_spider_plugin = 1 then
      -- spider plugin is present in mysql.plugin but not in
      -- information_schema.plugins.  Remove spider plugin entry
      -- in mysql.plugin first.
      delete from mysql.plugin where name = 'spider';
    end if;
    -- Install spider plugin
    if @win_plugin = 0 then 
      install plugin spider soname 'ha_spider.so';
    else
      install plugin spider soname 'ha_spider.dll';
    end if;
  end if;
end;//
delimiter ;
call mysql.spider_plugin_installer;
drop procedure mysql.spider_plugin_installer;
