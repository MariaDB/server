#!/usr/bin/env ruby
#
# Copyright(C) 2014  Kouhei Sutou <kou@clear-code.com>
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
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

require "optparse"
require "fileutils"
require "pathname"
require "open-uri"

class Uploader
  def initialize
    @dput_configuration_name = "groonga-ppa"
  end

  def run
    ensure_dput_configuration

    parse_command_line!

    ensure_mysql_version

    @required_groonga_version = required_groonga_version

    @code_names.each do |code_name|
      upload(code_name)
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
    @mysql_version = {}
    @code_names.each do |code_name|
      open("http://packages.ubuntu.com/#{code_name}/allpackages?format=txt.gz") do |file|
        file.each_line do |line|
          @mysql_version[code_name] = $1 if line =~ /\Amysql-server \((.+?)\).+/
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
    parser.on("--source-archive=ARCHIVE",
              "The source archive") do |source_archive|
      @source_archive = Pathname.new(source_archive).expand_path
    end
    parser.on("--code-names=CODE_NAME1,CODE_NAME2,CODE_NAME3,...", Array,
              "The target code names") do |code_names|
      @code_names = code_names
    end
    parser.on("--debian-directory=DIRECTORY",
              "The debian/ directory") do |debian_directory|
      @debian_directory = Pathname.new(debian_directory).expand_path
    end
    parser.on("--pgp-sign-key=KEY",
              "The PGP key to sign .changes and .dsc") do |pgp_sign_key|
      @pgp_sign_key = pgp_sign_key
    end
    parser.on("--pbuilder",
              "Use pbuilder for build check") do |pbuilder|
      @use_pbuilder = pbuilder
    end

    parser.parse!
  end

  def upload(code_name)
    in_temporary_directory do
      FileUtils.cp(@source_archive.to_s,
                   "#{@package}_#{@version}.orig.tar.gz")
      run_command("tar", "xf", @source_archive.to_s)
      directory_name = "#{@package}-#{@version}"
      Dir.chdir(directory_name) do
        FileUtils.cp_r(@debian_directory.to_s, "debian")
        deb_version = "#{current_deb_version.succ}~#{code_name}1"
        run_command("dch",
                    "--distribution", code_name,
                    "--newversion", deb_version,
                    "Build for #{code_name}.")
        run_command("sed",
                    "-i", "-e", "s,MYSQL_VERSION,#{@mysql_version[code_name]},",
                    "debian/control")
        run_command("debuild", "-S", "-sa", "-pgpg2", "-k#{@pgp_sign_key}")
        if @use_pbuilder
          run_command("pbuilder-dist", code_name, "build",
                      "../#{@package}_#{deb_version}.dsc")
        else
          run_command("dput", @dput_configuration_name,
                      "../#{@package}_#{deb_version}_source.changes")
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
