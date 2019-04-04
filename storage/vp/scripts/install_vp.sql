# Copyright (C) 2010 Kentoku Shiba
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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

# This SQL script creates system tables for SPIDER
#   or fixes incompatibilities if ones already exist.

-- Install a plugin and UDFs
drop procedure if exists mysql.vp_plugin_installer;
delimiter //
create procedure mysql.vp_plugin_installer()
begin
  set @win_plugin := IF(@@version_compile_os like 'Win%', 1, 0);
  set @have_vp_plugin := 0;
  select @have_vp_plugin := 1 from INFORMATION_SCHEMA.plugins where PLUGIN_NAME = 'VP';
  if @have_vp_plugin = 0 then 
    if @win_plugin = 0 then 
      install plugin vp soname 'ha_vp.so';
    else
      install plugin vp soname 'ha_vp.dll';
    end if;
  end if;
  set @have_vp_copy_tables_udf := 0;
  select @have_vp_copy_tables_udf := 1 from mysql.func where name = 'vp_copy_tables';
  if @have_vp_copy_tables_udf = 0 then
    if @win_plugin = 0 then 
      create function vp_copy_tables returns int soname 'ha_vp.so';
    else
      create function vp_copy_tables returns int soname 'ha_vp.dll';
    end if;
  end if;
end;//
delimiter ;
call mysql.vp_plugin_installer;
drop procedure mysql.vp_plugin_installer;
