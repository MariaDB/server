#!/usr/bin/env ruby

if ARGV.size < 1
  puts "Usage: #{$0} USER FILE ..."
  puts " e.g.: #{$0} kou mroonga-1.10.tar.gz ..."
  exit false
end

require "rubygems"
require "github_api"
require "mime/types"

user, *files = *ARGV

print "password[#{user}]: "
system("stty -echo")
password = STDIN.gets.chomp
system("stty echo")
puts

github = Github.new(:login => user, :password => password)
files.each do |file|
  content_type = MIME::Types.type_for(file)[0].to_s
  resource = github.repos.downloads.create("mroonga", "mroonga",
                                           :name => File.basename(file),
                                           :size => File.size(file),
                                           :description => File.basename(file),
                                           :content_type => content_type)
  p resource

  system("curl",
    "-F", "key=#{resource.path}",
    "-F", "acl=#{resource.acl}",
    "-F", "success_action_status=201",
    "-F", "Filename=#{resource.name}",
    "-F", "AWSAccessKeyId=#{resource.accesskeyid}",
    "-F", "Policy=#{resource.policy}",
    "-F", "Signature=#{resource.signature}",
    "-F", "Content-Type=#{resource.mime_type}",
    "-F", "file=@#{file}",
    resource.s3_url)
end
