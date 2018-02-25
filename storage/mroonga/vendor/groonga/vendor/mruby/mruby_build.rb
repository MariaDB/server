#!/usr/bin/env ruby

if ARGV.size != 5
  puts("Usage: #{$0} BUILD_COFNIG.RB MRUBY_SOURCE_DIR MRUBY_BUILD_DIR ONIGURUMA_INCLUDE_PATH TIMESTAMP_FILE")
  exit(false)
end

require "rbconfig"
require "fileutils"

build_config_rb = File.expand_path(ARGV.shift)
mruby_source_dir = ARGV.shift
mruby_build_dir = File.expand_path(ARGV.shift)
oniguruma_include_path = File.expand_path(ARGV.shift)
timestamp_file = File.expand_path(ARGV.shift)

FileUtils.rm_rf(mruby_build_dir)

Dir.chdir(mruby_source_dir) do
  unless system(RbConfig.ruby,
                "minirake",
                "MRUBY_CONFIG=#{build_config_rb}",
                "MRUBY_BUILD_DIR=#{mruby_build_dir}",
                "MRUBY_ONIGURUMA_INCLUDE_PATH=#{oniguruma_include_path}")
    exit(false)
  end
end

FileUtils.touch(timestamp_file)

FileUtils.cp("#{mruby_build_dir}/host/LEGAL", "./")

FileUtils.cp("#{mruby_build_dir}/host/mrblib/mrblib.c", "./")

File.open("mrbgems_init.c", "w") do |mrbgems_init|
  Dir.glob("#{mruby_build_dir}/host/mrbgems/**/gem_init.c") do |gem_init|
    mrbgems_init.puts(File.read(gem_init))
  end
end

mruby_compiler_dir = "#{mruby_build_dir}/host/mrbgems/mruby-compiler"
FileUtils.mkdir_p("mruby-compiler/core/")
FileUtils.cp("#{mruby_compiler_dir}/core/y.tab.c",
             "mruby-compiler/core/parse.c")

mruby_onig_regexp_dir = "#{mruby_build_dir}/mrbgems/mruby-onig-regexp"
FileUtils.mkdir_p("mruby-onig-regexp/")
FileUtils.cp_r("#{mruby_onig_regexp_dir}/src/", "mruby-onig-regexp/")

mruby_env_dir = "#{mruby_build_dir}/mrbgems/mruby-env"
FileUtils.mkdir_p("mruby-env/")
FileUtils.cp_r("#{mruby_env_dir}/src/", "mruby-env/")

mruby_io_dir = "#{mruby_build_dir}/mrbgems/mruby-io"
FileUtils.mkdir_p("mruby-io/")
FileUtils.cp_r("#{mruby_io_dir}/include/", "mruby-io/")
FileUtils.cp_r("#{mruby_io_dir}/src/", "mruby-io/")

mruby_file_stat_dir = "#{mruby_build_dir}/mrbgems/mruby-file-stat"
FileUtils.mkdir_p("mruby-file-stat/")
FileUtils.cp_r("#{mruby_file_stat_dir}/src/", "mruby-file-stat/")

mruby_dir_dir = "#{mruby_build_dir}/mrbgems/mruby-dir"
FileUtils.mkdir_p("mruby-dir/")
FileUtils.cp_r("#{mruby_dir_dir}/src/", "mruby-dir/")
