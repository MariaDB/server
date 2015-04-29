# README

## Name

groonga-normalizer-mysql

## Description

Groonga-normalizer-mysql is a Groonga plugin. It provides MySQL
compatible normalizers and a custom normalizers to Groonga.

Here are MySQL compatible normalizers:

* `NormalizerMySQLGeneralCI` for `utf8mb4_general_ci`
* `NormalizerMySQLUnicodeCI` for `utf8mb4_unicode_ci`
* `NormalizerMySQLUnicode520CI` for `utf8mb4_unicode_520_ci`

Here are custom normalizers:

* `NormalizerMySQLUnicodeCIExceptKanaCIKanaWithVoicedSoundMark`
   * It's based on `NormalizerMySQLUnicodeCI`
* `NormalizerMySQLUnicode520CIExceptKanaCIKanaWithVoicedSoundMark`
   * It's based on `NormalizerMySQLUnicode520CI`

They are self-descriptive name but long. They are variant normalizers
of `NormalizerMySQLUnicodeCI` and `NormalizerMySQLUnicode520CI`. They
have different behaviors. The followings are the different
behaviors. They describes with
`NormalizerMySQLUnicodeCIExceptKanaCIKanaWithVoicedSoundMark` but they
are true for
`NormalizerMySQLUnicode520CIExceptKanaCIKanaWithVoicedSoundMark`.

* `NormalizerMySQLUnicodeCI` normalizes all small Hiragana such as `ぁ`,
  `っ` to Hiragana such as `あ`, `つ`.
  `NormalizerMySQLUnicodeCIExceptKanaCIKanaWithVoicedSoundMark`
  doesn't normalize `ぁ` to `あ` nor `っ` to `つ`. `ぁ` and `あ` are
  different characters. `っ` and `つ` are also different characters.
  This behavior is described by `ExceptKanaCI` in the long name.  This
  following behaviors ared described by
  `ExceptKanaWithVoicedSoundMark` in the long name.
* `NormalizerMySQLUnicode` normalizes all Hiragana with voiced sound
  mark such as `が` to Hiragana without voiced sound mark such as `か`.
  `NormalizerMySQLUnicodeCIExceptKanaCIKanaWithVoicedSoundMark` doesn't
  normalize `が` to `か`. `が` and `か` are different characters.
* `NormalizerMySQLUnicode` normalizes all Hiragana with semi-voiced sound
  mark such as `ぱ` to Hiragana without semi-voiced sound mark such as `は`.
  `NormalizerMySQLUnicodeCIExceptKanaCIKanaWithVoicedSoundMark` doesn't
  normalize `ぱ` to `は`. `ぱ` and `は` are different characters.
* `NormalizerMySQLUnicode` normalizes all Katakana with voiced sound
  mark such as `ガ` to Katakana without voiced sound mark such as `カ`.
  `NormalizerMySQLUnicodeCIExceptKanaCIKanaWithVoicedSoundMark` doesn't
  normalize `ガ` to `カ`. `ガ` and `カ` are different characters.
* `NormalizerMySQLUnicode` normalizes all Katakana with semi-voiced sound
  mark such as `パ` to Hiragana without semi-voiced sound mark such as `ハ`.
  `NormalizerMySQLUnicodeCIExceptKanaCIKanaWithVoicedSoundMark` doesn't
  normalize `パ` to `ハ`. `パ` and `ハ` are different characters.
* `NormalizerMySQLUnicode` normalizes all halfwidth Katakana with
  voiced sound mark such as `ｶﾞ` to halfwidth Katakana without voiced
  sound mark such as `ｶ`.
  `NormalizerMySQLUnicodeCIExceptKanaCIKanaWithVoicedSoundMark`
  normalizes all halfwidth Katakana with voided sound mark such as `ｶﾞ`
  to fullwidth Katakana with voiced sound mark such as `ガ`.

`NormalizerMySQLUnicodeCIExceptKanaCIKanaWithVoicedSoundMark` and
`NormalizerMySQLUnicode520CIExceptKanaCIKanaWithVoicedSoundMark` and
are MySQL incompatible normalizers but they are useful for Japanese
text. For example, `ふらつく` and `ブラック` has different
means. `NormalizerMySQLUnicodeCI` identifies `ふらつく` with `ブラック`
but `NormalizerMySQLUnicodeCIExceptKanaCIKanaWithVoicedSoundMark`
doesn't identify them.

## Install

### Debian GNU/Linux

[Add apt-line for the Groonga deb package repository](http://groonga.org/docs/install/debian.html)
and install `groonga-normalizer-mysql` package:

    % sudo apt-get -y install groonga-normalizer-mysql

### Ubuntu

[Add apt-line for the Groonga deb package repository](http://groonga.org/docs/install/ubuntu.html)
and install `groonga-normalizer-mysql` package:

    % sudo apt-get -y install groonga-normalizer-mysql

### CentOS

Install `groonga-repository` package:

    % sudo rpm -ivh http://packages.groonga.org/centos/groonga-release-1.1.0-1.noarch.rpm
    % sudo yum makecache

Then install `groonga-normalizer-mysql` package:

    % sudo yum install -y groonga-normalizer-mysql

### Fedora

Install `groonga-repository` package:

    % sudo rpm -ivh http://packages.groonga.org/fedora/groonga-release-1.1.0-1.noarch.rpm
    % sudo yum makecache

Then install `groonga-normalizer-mysql` package:

    % sudo yum install -y groonga-normalizer-mysql

### OS X - Homebrew

Install `groonga-normalizer-mysql` package:

    % brew install groonga-normalizer-mysql

### Windows

You need to build from source. Here are build instructions.

#### Build system

Install the following build tools:

* [Microsoft Visual Studio 2010 Express](http://www.microsoft.com/japan/msdn/vstudio/express/): 2012 isn't tested yet.
* [CMake](http://www.cmake.org/)

#### Build Groonga

Download the latest Groonga source from [packages.groonga.org](http://packages.groonga.org/source/groonga/). Source file name is formatted as `groonga-X.Y.Z.zip`.

Extract the source and move to the source folder:

    > cd ...\groonga-X.Y.Z
    groonga-X.Y.Z>

Run CMake. Here is a command line to install Groonga to `C:\groonga` folder:

    groonga-X.Y.Z> cmake . -G "Visual Studio 12 Win64" -DCMAKE_INSTALL_PREFIX=C:\groonga

Build:

    groonga-X.Y.Z> cmake --build . --config Release

Install:

    groonga-X.Y.Z> cmake --build . --config Release --target Install

#### Build groonga-normalizer-mysql

Download the latest groonga-normalizer-mysql source from [packages.groonga.org](http://packages.groonga.org/source/groonga-normalizer-mysql/). Source file name is formatted as `groonga-normalizer-X.Y.Z.zip`.

Extract the source and move to the source folder:

    > cd ...\groonga-normalizer-mysql-X.Y.Z
    groonga-normalizer-mysql-X.Y.Z>

IMPORTANT!!!: Set `PKG_CONFIG_PATH` environment variable:

    groonga-normalizer-mysql-X.Y.Z> set PKG_CONFIG_PATH=C:\groongalocal\lib\pkgconfig

Run CMake. Here is a command line to install Groonga to `C:\groonga` folder:

    groonga-normalizer-mysql-X.Y.Z> cmake . -G "Visual Studio 12 Win64" -DCMAKE_INSTALL_PREFIX=C:\groonga

Build:

    groonga-normalizer-mysql-X.Y.Z> cmake --build . --config Release

Install:

    groonga-normalizer-mysql-X.Y.Z> cmake --build . --config Release --target Install

## Usage

First, you need to register `normalizers/mysql` plugin:

    groonga> register normalizers/mysql

Then, you can use `NormalizerMySQLGeneralCI` and
`NormalizerMySQLUnicodeCI` as normalizers:

    groonga> table_create Lexicon TABLE_PAT_KEY --default_tokenizer TokenBigram --normalizer NormalizerMySQLGeneralCI

## Dependencies

* Groonga >= 3.0.3

## Mailing list

* English: [groonga-talk@lists.sourceforge.net](https://lists.sourceforge.net/lists/listinfo/groonga-talk)
* Japanese: [groonga-dev@lists.sourceforge.jp](http://lists.sourceforge.jp/mailman/listinfo/groonga-dev)

## Thanks

* Alexander Barkov \<bar@udm.net\>: The author of
  `MYSQL_SOURCE/strings/ctype-utf8.c`.
* ...

## Authors

* Kouhei Sutou \<kou@clear-code.com\>

## License

LGPLv2 only. See doc/text/lgpl-2.0.txt for details.

This program uses normalization table defined in MySQL source code. So
this program is derived work of
`MYSQL_SOURCE/strings/ctype-utf8.c`. This program is the same license
as `MYSQL_SOURCE/strings/ctype-utf8.c` and it is licensed under LGPLv2
only.
