#!/usr/bin/env ruby
#
# Copyright(C) 2014-2016  Kouhei Sutou <kou@clear-code.com>
# Copyright(C) 2014  HAYASHI Kentaro <hayashi@clear-code.com>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License version 2.1 as published by the Free Software Foundation.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA

require "optparse"
require "fileutils"
require "pathname"
require "open-uri"

class Uploader
  def initialize
    @dput_configuration_name = "groonga-ppa"
    @use_pbuilder = false
  end

  def run
    ensure_dput_configuration

    parse_command_line!

    ensure_mysql_version

    @required_groonga_version = required_groonga_version

    @code_names.each do |code_name|
      mysql55_version = @mysql55_versions[code_name]
      mysql56_version = @mysql56_versions[code_name]
      mysql57_version = @mysql57_versions[code_name]
      mariadb10_0_version = @mariadb10_0_versions[code_name]
      if mysql55_version
        upload(code_name, "5.5", mysql55_version)
      end
      if mysql56_version
        upload(code_name, "5.6", mysql56_version)
      end
      if mysql57_version
        upload(code_name, "5.7", mysql57_version)
      end
      if mariadb10_0_version
        upload(code_name, "mariadb-10.0", mariadb10_0_version)
      end
    end
  end

  private
  def ensure_dput_configuration
    dput_cf_path = Pathname.new("~/.dput.cf").expand_path
    if dput_cf_path.exist?
      dput_cf_content = dput_cf_path.read
    else
      dput_cf_content = ""
    end
    dput_cf_content.each_line do |line|
      return if line.chomp == "[#{@dput_configuration_name}]"
    end

    dput_cf_path.open("w") do |dput_cf|
      dput_cf.puts(dput_cf_content)
      dput_cf.puts(<<-CONFIGURATION)
[#{@dput_configuration_name}]
fqdn = ppa.launchpad.net
method = ftp
incoming = ~groonga/ppa/ubuntu/
login = anonymous
allow_unsigned_uploads = 0
      CONFIGURATION
    end
  end

  def ensure_mysql_version
    @mysql_versions = {}
    @mysql55_versions = {}
    @mysql56_versions = {}
    @mysql57_versions = {}
    @mariadb10_0_versions = {}
    @code_names.each do |code_name|
      source_names = [code_name, "#{code_name}-updates"]
      source_names.each do |source_name|
        allpackages_url =
          "http://packages.ubuntu.com/#{source_name}/allpackages?format=txt.gz"
        open(allpackages_url) do |file|
          file.each_line do |line|
            case line
            when /\Amysql-server \((.+?)[\s)]/
              @mysql_versions[code_name] = $1
            when /\Amysql-server-5\.5 \((.+?)[\s)]/
              @mysql55_versions[code_name] = $1
            when /\Amysql-server-5\.6 \((.+?)[\s)]/
              @mysql56_versions[code_name] = $1
            when /\Amysql-server-5\.7 \((.+?)[\s)]/
              @mysql57_versions[code_name] = $1
            when /\Amariadb-server-10\.0 \((.+?)[\s)]/
              @mariadb10_0_versions[code_name] = $1
            end
          end
        end
      end
    end
  end

  def parse_command_line!
    parser = OptionParser.new
    parser.on("--package=NAME",
              "The package name") do |name|
      @package = name
    end
    parser.on("--version=VERSION",
              "The version") do |version|
      @version = version
    end
    parser.on("--source-archive-directory=DIRECTORY",
              "The directory that has source archives") do |directory|
      @source_archive_directory = Pathname.new(directory).expand_path
    end
    parser.on("--code-names=CODE_NAME1,CODE_NAME2,CODE_NAME3,...", Array,
              "The target code names") do |code_names|
      @code_names = code_names
    end
    parser.on("--debian-base-directory=DIRECTORY",
              "The directory that has debianXX/ directory") do |directory|
      @debian_base_directory = Pathname.new(directory).expand_path
    end
    parser.on("--ppa=PPA",
              "The personal package archive name (groonga-ppa or groonga-nightly") do |ppa|
      @dput_configuration_name = ppa
    end
    parser.on("--pgp-sign-key=KEY",
              "The PGP key to sign .changes and .dsc") do |pgp_sign_key|
      @pgp_sign_key = pgp_sign_key
    end
    parser.on("--[no-]pbuilder",
              "Use pbuilder for build check") do |use_pbuilder|
      @use_pbuilder = use_pbuilder
    end

    parser.parse!
  end

  def upload(code_name, mysql_short_version, mysql_version)
    default_mysql_version = (@mysql_versions[code_name] == mysql_version)
    deb_package_name = "#{@package}-#{mysql_short_version}"
    in_temporary_directory do
      source_archive =
        @source_archive_directory + "#{deb_package_name}_#{@version}.orig.tar.gz"
      run_command("tar", "xf", source_archive.to_s)
      directory_name = "#{deb_package_name}-#{@version}"
      Dir.chdir(directory_name) do
        debian_directory =
          @debian_base_directory + "debian-#{mysql_short_version}"
        FileUtils.cp_r(debian_directory.to_s, "debian")
        deb_version = "#{current_deb_version.succ}~#{code_name}1"
        run_command("dch",
                    "--distribution", code_name,
                    "--newversion", deb_version,
                    "Build for #{code_name}.")
        remove_versionless_mroonga = true
        if default_mysql_version or mysql_short_version.start_with?("mariadb-")
          remove_versionless_mroonga = false
        end
        if remove_versionless_mroonga
          control_content = File.read("debian/control")
          File.open("debian/control", "w") do |control|
            in_mysql_server_mroonga = false
            control_content.each_line do |line|
              case line.chomp
              when ""
                if in_mysql_server_mroonga
                  in_mysql_server_mroonga = false
                else
                  control.print(line)
                end
              when "Package: mysql-server-mroonga"
                in_mysql_server_mroonga = true
              else
                next if in_mysql_server_mroonga
                control.print(line)
              end
            end
          end
        end
        run_command("sed",
                    "-i", "-e",
                    "s,MYSQL_VERSION\\|MARIADB_VERSION,#{mysql_version},",
                    "debian/control")
        run_command("debuild",
                    "--no-lintian",
                    # Workaround for Launchpad. Launchpad doesn't accept
                    # .buildinfo yet.
                    # See also: https://bugs.debian.org/cgi-bin/bugreport.cgi?bug=853795
                    "--buildinfo-option=-O",
                    "-d",
                    "-S",
                    "-sa",
                    "-pgpg2",
                    "-k#{@pgp_sign_key}")
        if @use_pbuilder
          run_command("pbuilder-dist", code_name, "build",
                      "../#{deb_package_name}_#{deb_version}.dsc")
        else
          run_command("dput", @dput_configuration_name,
                      "../#{deb_package_name}_#{deb_version}_source.changes")
        end
      end
    end
  end

  def required_groonga_version
    File.read("../../required_groonga_version").lines.first.chomp
  end

  def current_deb_version
    /\((.+)\)/ =~ File.read("debian/changelog").lines.first
    $1
  end

  def in_temporary_directory
    name = "tmp"
    FileUtils.rm_rf(name)
    FileUtils.mkdir_p(name)
    Dir.chdir(name) do
      yield
    end
  end

  def run_command(*command_line)
    unless system(*command_line)
      raise "failed to run command: #{command_line.join(' ')}"
    end
  end
end

uploader = Uploader.new
uploader.run
