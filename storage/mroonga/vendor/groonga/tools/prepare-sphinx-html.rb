#!/usr/bin/env ruby
# -*- coding: utf-8 -*-

if ARGV.size != 2
  puts "Usage: #{$0} SOURCE_DIR DEST_DIR"
  exit(false)
end

require 'pathname'

def fix_link(text, extension, language)
  send("fix_#{extension}_link", text, language)
end

def fix_link_path(text)
  text.gsub(/\b_(sources|static|images)\b/, '\1')
end

def fix_language_link(url, language)
  url.gsub(/\A((?:\.\.\/){2,})([a-z]{2})\/html\//) do
    relative_base_path = $1
    link_language = $2
    close_quote = $3
    if language == "en"
      relative_base_path = relative_base_path.gsub(/\A\.\.\//, '')
    end
    if link_language != "en"
      relative_base_path += "#{link_language}/"
    end
    "#{relative_base_path}docs/"
  end
end

def fix_html_link(html, language)
  html = html.gsub(/(href|src)="(.+?)"/) do
    attribute = $1
    link = $2
    link = fix_link_path(link)
    link = fix_language_link(link, language)
    "#{attribute}=\"#{link}\""
  end
  html.gsub(/(id="top-link" href=)"(.+?)"/) do
    prefix = $1
    top_path = $2.gsub(/\/index\.html\z/, '/')
    top_path = "./" if ["index.html", "#"].include?(top_path)
    "#{prefix}\"#{top_path}../\""
  end
end

def add_language_annotation_to_source_label(html, language)
  return html unless language == "ja"
  html.gsub(/>(ソースコードを表示)</) do
    label = $1
    ">#{label}（英語）<"
  end
end

def fix_js_link(js, language)
  fix_link_path(js)
end

LANGUAGE_TO_LOCALE = {
  "ja" => "ja_JP",
  "en" => "en_US",
}

def insert_facebook_html_header(html)
  html.gsub(/<\/head>/) do
    <<-HTML
    <meta property="fb:page_id" content="201193596592346" /><!-- groonga -->
    <meta property="fb:admins" content="664204556" /><!-- kouhei.sutou -->
    <meta property="og:type" content="product" />
    <meta property="og:image" content="http://groonga.org/images/logos/groonga-icon-full-size.png" />
    <meta property="og:site_name" content="groonga" />

    <link rel="stylesheet" href="/css/sphinx.css" type="text/css" />
  </head>
    HTML
  end
end

def insert_facebook_html_fb_root(html)
  html.gsub(/<body>/) do
    <<-HTML
  <body>
    <div id="fb-root"></div>
    HTML
  end
end

def insert_facebook_html_buttons(html)
  html.gsub(/(<div class="other-language-links">)/) do
    <<-HTML
    <div class="facebook-buttons">
      <fb:like href="http://www.facebook.com/pages/groonga/201193596592346"
               layout="standard"
               width="290"></fb:like>
    </div>
    #{$1}
    HTML
  end
end

def insert_facebook_html_footer(html, language)
  locale = LANGUAGE_TO_LOCALE[language]
  raise "unknown locale for language #{language.inspect}" if locale.nil?
  html.gsub(/<\/body>/) do
    <<-HTML
    <script src="http://connect.facebook.net/#{locale}/all.js"></script>

    <script>
      FB.init({
         appId  : null,
         status : true, // check login status
         cookie : true, // enable cookies to allow the server to access the session
         xfbml  : true  // parse XFBML
      });
    </script>
  </body>
    HTML
  end
end

def insert_facebook_html(html, language)
  html = insert_facebook_html_header(html)
  html = insert_facebook_html_fb_root(html)
  html = insert_facebook_html_buttons(html)
  html = insert_facebook_html_footer(html, language)
  html
end

source_dir, dest_dir = ARGV

source_dir = Pathname.new(source_dir)
dest_dir = Pathname.new(dest_dir)

language_dirs = []
source_dir.each_entry do |top_level_path|
  language_dirs << top_level_path if /\A[a-z]{2}\z/ =~ top_level_path.to_s
end

language_dirs.each do |language_dir|
  language = language_dir.to_s
  language_source_dir = source_dir + language_dir + "html"
  language_dest_dir = dest_dir + language_dir
  language_source_dir.find do |source_path|
    relative_path = source_path.relative_path_from(language_source_dir)
    dest_path = language_dest_dir + relative_path
    if source_path.directory?
      dest_path.mkpath
    else
      case source_path.extname
      when ".html", ".js"
        content = source_path.read
        extension = source_path.extname.gsub(/\A\./, '')
        content = fix_link(content, extension, language)
        if extension == "html"
          content = insert_facebook_html(content, language)
          content = add_language_annotation_to_source_label(content, language)
        end
        dest_path.open("wb") do |dest|
          dest.print(content.strip)
        end
        FileUtils.touch(dest_path, :mtime => source_path.mtime)
      else
        case source_path.basename.to_s
        when ".buildinfo"
          # ignore
        else
          FileUtils.cp(source_path, dest_path, :preserve => true)
        end
      end
    end
  end
end

dest_dir.find do |dest_path|
  if dest_path.directory? and /\A_/ =~ dest_path.basename.to_s
    normalized_dest_path = dest_path + ".."
    normalized_dest_path += dest_path.basename.to_s.gsub(/\A_/, '')
    FileUtils.mv(dest_path, normalized_dest_path)
  end
end
