MRuby::Build.new do |conf|
  if ENV['VisualStudioVersion'] || ENV['VSINSTALLDIR']
    toolchain :visualcpp
  else
    toolchain :gcc
  end

  oniguruma_include_path = ENV["MRUBY_ONIGURUMA_INCLUDE_PATH"]
  if oniguruma_include_path
    conf.cc.include_paths << oniguruma_include_path
  end

  enable_debug

  conf.gem :core => "mruby-compiler"
  conf.gem :core => "mruby-sprintf"
  conf.gem :core => "mruby-print"
  conf.gem :core => "mruby-math"
  conf.gem :core => "mruby-time"
  conf.gem :core => "mruby-struct"
  conf.gem :core => "mruby-enum-ext"
  conf.gem :core => "mruby-string-ext"
  conf.gem :core => "mruby-numeric-ext"
  conf.gem :core => "mruby-array-ext"
  conf.gem :core => "mruby-hash-ext"
  conf.gem :core => "mruby-range-ext"
  conf.gem :core => "mruby-proc-ext"
  conf.gem :core => "mruby-symbol-ext"
  conf.gem :core => "mruby-random"
  conf.gem :core => "mruby-object-ext"
  conf.gem :core => "mruby-objectspace"
  conf.gem :core => "mruby-fiber"
  conf.gem :core => "mruby-enumerator"
  conf.gem :core => "mruby-enum-lazy"
  conf.gem :core => "mruby-toplevel-ext"
  conf.gem :core => "mruby-kernel-ext"

  conf.gem :github => "mattn/mruby-onig-regexp",
           :checksum_hash => "12f573cb327aa50834c3a549f62995f44edd3172"
  conf.gem :github => "iij/mruby-env",
           :checksum_hash => "57f0d737a4ece49dc5b6f1c7ee09b0bc8f8adf87"
  conf.gem :github => "iij/mruby-io",
           :checksum_hash => "69623078a86b45617a6fdbe0238c147e280ad9db"
  conf.gem :github => "kou/mruby-pp",
           :checksum_hash => "ddda20ca273ba532f2025d4ff7ddc8bb223ad8c2"
  conf.gem :github => "kou/mruby-slop",
           :checksum_hash => "752d1a3e2bc4fdc40ee92d668812a99c8fc5e1cc"
  conf.gem :github => "ksss/mruby-file-stat",
           :checksum_hash => "2d3ea9b5d59d2b41133228a71c110b75cb30a31e"
  conf.gem :github => "kou/mruby-tsort",
           :checksum_hash => "6d7f5a56ac7a90847f84186ce1dbc780e41928dc"
  conf.gem :github => "iij/mruby-dir",
           :checksum_hash => "14bc5c3e51eac16ebc9075b7b62132a0cf5ae724"
end
