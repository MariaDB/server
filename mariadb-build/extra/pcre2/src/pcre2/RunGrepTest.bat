@echo off

:: Run pcre2grep tests. The assumption is that the PCRE2 tests check the library
:: itself. What we are checking here is the file handling and options that are
:: supported by pcre2grep. This script must be run in the build directory.
:: (jmh: I've only tested in the main directory, using my own builds.)

setlocal enabledelayedexpansion

:: Remove any non-default colouring that the caller may have set.

set PCRE2GREP_COLOUR=
set PCRE2GREP_COLOR=
set PCREGREP_COLOUR=
set PCREGREP_COLOR=
set GREP_COLORS=
set GREP_COLOR=

:: Remember the current (build) directory and set the program to be tested.

set builddir="%CD%"

if [%pcre2grep%]==[] set pcre2grep=%builddir%\pcre2grep.exe
if [%pcre2test%]==[] set pcre2test=%builddir%\pcre2test.exe

if NOT exist %pcre2grep% (
  echo ** %pcre2grep% does not exist.
  exit /b 1
)

if NOT exist %pcre2test% (
  echo ** %pcre2test% does not exist.
  exit /b 1
)

for /f "delims=" %%a in ('"%pcre2grep%" -V') do set pcre2grep_version=%%a
echo Testing %pcre2grep_version%

:: Set up a suitable "diff" command for comparison. Some systems have a diff
:: that lacks a -u option. Try to deal with this; better do the test for the -b
:: option as well. Use FC if there's no diff, taking care to ignore equality.

set cf=
set cfout=
diff -b  nul nul 2>nul && set cf=diff -b
diff -u  nul nul 2>nul && set cf=diff -u
diff -ub nul nul 2>nul && set cf=diff -ub
if NOT defined cf (
  set cf=fc /n
  set "cfout=>testcf || (type testcf & cmd /c exit /b 1)"
)

:: Set srcdir to the current or parent directory, whichever one contains the
:: test data. Subsequently, we run most of the pcre2grep tests in the source
:: directory so that the file names in the output are always the same.

if NOT defined srcdir set srcdir=.
if NOT exist %srcdir%\testdata\ (
  if exist testdata\ (
    set srcdir=.
  ) else if exist ..\testdata\ (
    set srcdir=..
  ) else if exist ..\..\testdata\ (
    set srcdir=..\..
  ) else (
    echo Cannot find the testdata directory
    exit /b 1
  )
)

:: Check for the availability of UTF-8 support

%pcre2test% -C unicode >nul
set utf8=%ERRORLEVEL%

:: Check default newline convention. If it does not include LF, force LF.

for /f %%a in ('"%pcre2test%" -C newline') do set nl=%%a
if NOT "%nl%" == "LF" if NOT "%nl%" == "ANY" if NOT "%nl%" == "ANYCRLF" (
  set pcre2grep=%pcre2grep% -N LF
  echo Default newline setting forced to LF
)

:: Create a simple printf via cscript/JScript (an actual printf may translate
:: LF to CRLF, which this one does not).  We only support the barebones we need:
:: \r, \n, \0, and %s (but only once).

echo WScript.StdOut.Write(WScript.Arguments(0).replace(/\\r/g, "\r").replace(/\\n/g, "\n").replace(/\\0/g, "\x00").replace(/%%s/g, function() { return WScript.Arguments(1) })) >printf.js
set printf=cscript //nologo printf.js

:: Create a simple 'tr' via cscript/JScript.
echo WScript.StdOut.Write(WScript.StdIn.ReadAll().replace(/\x00/g, "@")) >trnull.js
set trnull=cscript //nologo trnull.js

:: ------ Normal tests ------

echo Testing pcre2grep main features

echo ---------------------------- Test 1 ------------------------------>testtrygrep
(pushd %srcdir% & %pcre2grep% PATTERN ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 2 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% "^PATTERN" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 3 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% -in PATTERN ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 4 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% -ic PATTERN ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 5 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% -in PATTERN ./testdata/grepinput ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 6 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% -inh PATTERN ./testdata/grepinput ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 7 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% -il PATTERN ./testdata/grepinput ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 8 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% -l PATTERN ./testdata/grepinput ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 9 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% -q PATTERN ./testdata/grepinput ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 10 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -q NEVER-PATTERN ./testdata/grepinput ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 11 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -vn pattern ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 12 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -ix pattern ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 13 ----------------------------->>testtrygrep
echo seventeen >testtemp1grep
(pushd %srcdir% & %pcre2grep% -f./testdata/greplist -f %builddir%\testtemp1grep ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 14 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -w pat ./testdata/grepinput ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 15 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% "abc^*" ./testdata/grepinput & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 16 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% abc ./testdata/grepinput ./testdata/nonexistfile & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 17 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -M "the\noutput" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 18 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -Mn "(the\noutput|dog\.\n--)" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 19 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -Mix "Pattern" ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 20 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -Mixn "complete pair\nof lines" ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 21 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -nA3 "four" ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 22 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -nB3 "four" ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 23 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -C3 "four" ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 24 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -A9 "four" ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 25 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -nB9 "four" ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 26 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -A9 -B9 "four" ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 27 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -A10 "four" ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 28 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -nB10 "four" ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 29 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -C12 -B10 "four" ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 30 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -inB3 "pattern" ./testdata/grepinput ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 31 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -inA3 "pattern" ./testdata/grepinput ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 32 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -L "fox" ./testdata/grepinput ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 33 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% "fox" ./testdata/grepnonexist & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 34 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -s "fox" ./testdata/grepnonexist & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 35 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -L -r --include=grepinputx --include grepinput8 --exclude-dir="^\." "fox" ./testdata | sort & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 36 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -L -r --include="grepinput[^C]" --exclude "grepinput$" --exclude="grepinput(Bad)?8" --exclude=grepinputM --exclude=grepinputUN --exclude-dir="^\." "fox" ./testdata | sort & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 37 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep%  "^(a+)*\d" ./testdata/grepinput & popd) >>testtrygrep 2>teststderrgrep
echo RC=^%ERRORLEVEL%>>testtrygrep
echo ======== STDERR ========>>testtrygrep
type teststderrgrep >>testtrygrep

echo ---------------------------- Test 38 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% ">\x00<" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 39 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% -A1 "before the binary zero" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 40 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% -B1 "after the binary zero" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 41 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% -B1 -o "\w+ the binary zero" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 42 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% -B1 -onH "\w+ the binary zero" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 43 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% -on "before|zero|after" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 44 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% -on -e before -ezero -e after ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 45 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% -on -f ./testdata/greplist -e binary ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 46 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% -e "unopened)" -e abc ./testdata/grepinput & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep
(pushd %srcdir% & %pcre2grep% -eabc -e "(unclosed" ./testdata/grepinput & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep
(pushd %srcdir% & %pcre2grep% -eabc -e xyz -e "[unclosed" ./testdata/grepinput & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep
(pushd %srcdir% & %pcre2grep% --regex=123 -eabc -e xyz -e "[unclosed" ./testdata/grepinput & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 47 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% -Fx AB.VE^

elephant ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 48 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% -F AB.VE^

elephant ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 49 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% -F -e DATA -e AB.VE^

elephant ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 50 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% "^(abc|def|ghi|jkl)" ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 51 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% -Mv "brown\sfox" ./testdata/grepinputv & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 52 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% --colour=always jumps ./testdata/grepinputv & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 53 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% --file-offsets "before|zero|after" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 54 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% --line-offsets "before|zero|after" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 55 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -f./testdata/greplist --color=always ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 56 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -c --exclude=grepinputC lazy ./testdata/grepinput* & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 57 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -c -l --exclude=grepinputC lazy ./testdata/grepinput* & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 58 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --regex=PATTERN ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 59 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --regexp=PATTERN ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 60 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --regex PATTERN ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 61 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --regexp PATTERN ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 62 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --match-limit=1000 --no-jit -M "This is a file(.|\R)*file." ./testdata/grepinput & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 63 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --recursion-limit=1000 --no-jit -M "This is a file(.|\R)*file." ./testdata/grepinput & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 64 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% -o1 "(?<=PAT)TERN (ap(pear)s)" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 65 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% -o2 "(?<=PAT)TERN (ap(pear)s)" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 66 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% -o3 "(?<=PAT)TERN (ap(pear)s)" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 67 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% -o12 "(?<=PAT)TERN (ap(pear)s)" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 68 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% --only-matching=2 "(?<=PAT)TERN (ap(pear)s)" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 69 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -vn --colour=always pattern ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 70 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --color=always -M "triple:\t.*\n\n" ./testdata/grepinput3 & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep
(pushd %srcdir% & %pcre2grep% --color=always -M -n "triple:\t.*\n\n" ./testdata/grepinput3 & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep
(pushd %srcdir% & %pcre2grep% -M "triple:\t.*\n\n" ./testdata/grepinput3 & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep
(pushd %srcdir% & %pcre2grep% -M -n "triple:\t.*\n\n" ./testdata/grepinput3 & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 71 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -o "^01|^02|^03" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 72 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --color=always "^01|^02|^03" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 73 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -o --colour=always "^01|^02|^03" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 74 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -o "^01|02|^03" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 75 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --color=always "^01|02|^03" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 76 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -o --colour=always "^01|02|^03" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 77 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -o "^01|^02|03" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 78 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --color=always "^01|^02|03" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 79 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -o --colour=always "^01|^02|03" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 80 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -o "\b01|\b02" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 81 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --color=always "\b01|\b02" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 82 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -o --colour=always "\b01|\b02" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 83 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --buffer-size=10 --max-buffer-size=100 "^a" ./testdata/grepinput3 & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 84 ----------------------------->>testtrygrep
echo testdata/grepinput3 >testtemp1grep
(pushd %srcdir% & %pcre2grep% --file-list ./testdata/grepfilelist --file-list %builddir%\testtemp1grep "fox|complete|t7" & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 85 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --file-list=./testdata/grepfilelist "dolor" ./testdata/grepinput3 & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 86 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% "dog" ./testdata/grepbinary & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 87 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% "cat" ./testdata/grepbinary & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 88 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -v "cat" ./testdata/grepbinary & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 89 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -I "dog" ./testdata/grepbinary & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 90 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --binary-files=without-match "dog" ./testdata/grepbinary & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 91 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -a "dog" ./testdata/grepbinary & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 92 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --binary-files=text "dog" ./testdata/grepbinary & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 93 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --text "dog" ./testdata/grepbinary & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 94 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -L -r --include=grepinputx --include grepinput8 "fox" ./testdata/grepinput* | sort & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 95 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --file-list ./testdata/grepfilelist --exclude grepinputv "fox|complete" & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 96 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -L -r --include-dir=testdata --exclude "^^(?^!grepinput)" --exclude=grepinput[MCU] "fox" ./test* | sort & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 97 ----------------------------->>testtrygrep
echo grepinput$>testtemp1grep
echo grepinput8>>testtemp1grep
echo grepinputBad8>>testtemp1grep
(pushd %srcdir% & %pcre2grep% -L -r --include=grepinput --exclude=grepinput[MCU] --exclude-from %builddir%\testtemp1grep --exclude-dir="^\." "fox" ./testdata | sort & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 98 ----------------------------->>testtrygrep
echo grepinput$>testtemp1grep
echo grepinput8>>testtemp1grep
echo grepinputBad8>>testtemp1grep
(pushd %srcdir% & %pcre2grep% -L -r --exclude=grepinput3 --exclude=grepinput[MCU] --include=grepinput --exclude-from %builddir%\testtemp1grep --exclude-dir="^\." "fox" ./testdata | sort & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 99 ----------------------------->>testtrygrep
echo grepinput$>testtemp1grep
echo grepinput8>testtemp2grep
echo grepinputBad8>>testtemp1grep
(pushd %srcdir% & %pcre2grep% -L -r --include grepinput --exclude=grepinput[MCU] --exclude-from %builddir%\testtemp1grep --exclude-from=%builddir%\testtemp2grep --exclude-dir="^\." "fox" ./testdata | sort & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 100 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% -Ho2 --only-matching=1 -o3 "(\w+) binary (\w+)(\.)?" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 101 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% -o3 -Ho2 -o12 --only-matching=1 -o3 --colour=always --om-separator="|" "(\w+) binary (\w+)(\.)?" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 102 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -n "^$" ./testdata/grepinput3 & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 103 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --only-matching "^$" ./testdata/grepinput3 & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 104 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -n --only-matching "^$" ./testdata/grepinput3 & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 105 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --colour=always "ipsum|" ./testdata/grepinput3 & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 106 ----------------------------->>testtrygrep
(pushd %srcdir% & echo a| %pcre2grep% -M "|a" & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 107 ----------------------------->>testtrygrep
echo a>testtemp1grep
echo aaaaa>>testtemp1grep
(pushd %srcdir% & %pcre2grep%  --line-offsets --allow-lookaround-bsk "(?<=\Ka)" %builddir%\testtemp1grep & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 108 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% -lq PATTERN ./testdata/grepinput ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 109 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -cq --exclude=grepinputC lazy ./testdata/grepinput* & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 110 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --om-separator / -Mo0 -o1 -o2 "match (\d+):\n (.)\n" testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 111 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --line-offsets -M "match (\d+):\n (.)\n" testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 112 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --file-offsets -M "match (\d+):\n (.)\n" testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 113 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --total-count --exclude=grepinputC "the" testdata/grepinput* & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 114 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -tc --exclude=grepinputC "the" testdata/grepinput* & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 115 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -tlc --exclude=grepinputC "the" testdata/grepinput* & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 116 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --exclude=grepinput[MCU] -th "the" testdata/grepinput* & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 117 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -tch --exclude=grepinputC "the" testdata/grepinput* & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 118 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -tL --exclude=grepinputC "the" testdata/grepinput* & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 119 ----------------------------->>testtrygrep
%printf% "123\n456\n789\n---abc\ndef\nxyz\n---\n" >testNinputgrep
%pcre2grep% -Mo "(\n|[^-])*---" testNinputgrep >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 120 ------------------------------>>testtrygrep
(pushd %srcdir% & %pcre2grep% -HO "$0:$2$1$3" "(\w+) binary (\w+)(\.)?" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep
(pushd %srcdir% & %pcre2grep% -HO "$&:$2$1$3" "(\w+) binary (\w+)(\.)?" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep
(pushd %srcdir% & %pcre2grep% -m 1 -O "$0:$a$b$e$f$r$t$v" "(\w+) binary (\w+)(\.)?" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep
(pushd %srcdir% & %pcre2grep% -HO "${X}" "(\w+) binary (\w+)(\.)?" ./testdata/grepinput & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep
(pushd %srcdir% & %pcre2grep% -HO "XX$" "(\w+) binary (\w+)(\.)?" ./testdata/grepinput & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep
(pushd %srcdir% & %pcre2grep% -O "$x{12345678}" "(\w+) binary (\w+)(\.)?" ./testdata/grepinput & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep
(pushd %srcdir% & %pcre2grep% -O "$x{123Z" "(\w+) binary (\w+)(\.)?" ./testdata/grepinput & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep
(pushd %srcdir% & %pcre2grep% --output "$x{1234}" "(\w+) binary (\w+)(\.)?" ./testdata/grepinput & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 121 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -F "\E and (regex)" testdata/grepinputv & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 122 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -w "cat|dog" testdata/grepinputv & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 123 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -w "dog|cat" testdata/grepinputv & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 124 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -Mn --colour=always "start[\s]+end" testdata/grepinputM & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep
(pushd %srcdir% & %pcre2grep% -Mn --colour=always -A2 "start[\s]+end" testdata/grepinputM & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep
(pushd %srcdir% & %pcre2grep% -Mn "start[\s]+end" testdata/grepinputM & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep
(pushd %srcdir% & %pcre2grep% -Mn -A2 "start[\s]+end" testdata/grepinputM & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 125 ----------------------------->>testtrygrep
%printf% "abcd\n" >testNinputgrep
%pcre2grep% --colour=always --allow-lookaround-bsk "(?<=\K.)" testNinputgrep >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep
%pcre2grep% --colour=always --allow-lookaround-bsk "(?=.\K)" testNinputgrep >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep
%pcre2grep% --colour=always --allow-lookaround-bsk "(?<=\K[ac])" testNinputgrep >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep
%pcre2grep% --colour=always --allow-lookaround-bsk "(?=[ac]\K)" testNinputgrep >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep
set GREP_COLORS=ms=1;20
%pcre2grep% --colour=always --allow-lookaround-bsk "(?=[ac]\K)" testNinputgrep >>testtrygrep
set GREP_COLORS=
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 126 ----------------------------->>testtrygrep
%printf% "Next line pattern has binary zero\nABC\0XYZ\n" >testtemp1grep
%printf% "ABC\0XYZ\nABCDEF\nDEFABC\n" >testtemp2grep
%pcre2grep% -a -f testtemp1grep testtemp2grep >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep
%printf% "Next line pattern is erroneous.\n^abc)(xy" >testtemp1grep
%pcre2grep% -a -f testtemp1grep testtemp2grep >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 127 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -o --om-capture=0 "pattern()()()()" testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 128 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -m1M -o1 --om-capture=0 "pattern()()()()" testdata/grepinput & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 129 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -m 2 "fox" testdata/grepinput & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 130 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -o -m2 "fox" testdata/grepinput & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 131 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -oc -m2 "fox" testdata/grepinput & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 132 ----------------------------->>testtrygrep
:: The Unix tests use fd3 here, but Windows only has StdIn/StdOut/StdErr (which, at the kernel
:: level, are not even numbered). Use a subshell instead.
(pushd %srcdir% & (%pcre2grep% -m1 -A3 "^match" & echo ---& %pcre2grep% -m1 ".*") <testdata/grepinput & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 133 ----------------------------->>testtrygrep
:: The Unix tests use fd3 here, but Windows only has StdIn/StdOut/StdErr (which, at the kernel
:: level, are not even numbered). Use a subshell instead.
(pushd %srcdir% & (%pcre2grep% -m1 -A3 "^match" & echo ---& %pcre2grep% -m1 -A3 "^match") <testdata/grepinput & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 134 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --max-count=1 -nH -O "=$x{41}$x423$o{103}$o1045=" "fox" - & popd) <%srcdir%\testdata\grepinputv >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 135 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -HZ "word" ./testdata/grepinputv & popd) | %trnull% >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep
(pushd %srcdir% & %pcre2grep% -lZ "word" ./testdata/grepinputv ./testdata/grepinputv & popd) | %trnull% >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep
(pushd %srcdir% & %pcre2grep% -A 1 -B 1 -HZ "word" ./testdata/grepinputv & popd) | %trnull% >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep
(pushd %srcdir% & %pcre2grep% -MHZn "start[\s]+end" testdata/grepinputM & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 136 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -m1MK -o1 --om-capture=0 "pattern()()()()" testdata/grepinput & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep
(pushd %srcdir% & %pcre2grep% --max-count=1MK -o1 --om-capture=0 "pattern()()()()" testdata/grepinput & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 137 ----------------------------->>testtrygrep
%printf% "Last line\nhas no newline" >testtemp1grep
%pcre2grep% -A1 Last testtemp1grep >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 138 ----------------------------->>testtrygrep
%printf% "AbC\nAbC\nAbC\nAbC\nAbC\nAbC\nAbC\nAbC\nAbC\nAbC\nAbC\nAbC\nAbC\nAbC\nAbC\nAbC\nAbC\nAbC\nAbC\nAbC\nAbC\nAbC\nAbC\nAbC\n" >testtemp1grep
%pcre2grep% --no-jit --heap-limit=0 b testtemp1grep >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 139 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --line-buffered "fox" testdata/grepinputv & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 140 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --buffer-size=10 -A1 "brown" testdata/grepinputv & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 141 ----------------------------->>testtrygrep
%printf% "%%s\testdata\grepinputv\n-\n" "%srcdir%" >testtemp1grep
%printf% "This is a line from stdin." >testtemp2grep
%pcre2grep% --file-list testtemp1grep "line from stdin" <testtemp2grep >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 142 ----------------------------->>testtrygrep
%printf% "/does/not/exist\n" >testtemp1grep
%printf% "This is a line from stdin." >testtemp2grep
%pcre2grep% --file-list testtemp1grep "line from stdin" >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 143 ----------------------------->>testtrygrep
%printf% "fox|cat" >testtemp1grep
%pcre2grep% -f - %srcdir%\testdata\grepinputv <testtemp1grep >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 144 ----------------------------->>testtrygrep
%pcre2grep% -f /non/exist %srcdir%\testdata\grepinputv >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 145 ----------------------------->>testtrygrep
%printf% "*meta*\rdog." >testtemp1grep
%pcre2grep% -Ncr -F -f testtemp1grep %srcdir%\testdata\grepinputv >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 146 ----------------------------->>testtrygrep
%printf% "A123B" >testtemp1grep
%pcre2grep% -H -e "123|fox" - <testtemp1grep >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep
%pcre2grep% -h -e "123|fox" - %srcdir%\testdata\grepinputv <testtemp1grep >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep
%pcre2grep% - %srcdir%\testdata\grepinputv <testtemp1grep >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 147 ----------------------------->>testtrygrep
%pcre2grep% -e "123|fox" -- -nonfile >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 148 ----------------------------->>testtrygrep
%pcre2grep% --nonexist >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep
%pcre2grep% -n-n-bad >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep
%pcre2grep% --context >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep
%pcre2grep% --only-matching --output=xx >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep
%pcre2grep% --colour=badvalue >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep
%pcre2grep% --newline=badvalue >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep
%pcre2grep% -d badvalue >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep
%pcre2grep% -D badvalue >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep
%pcre2grep% --buffer-size=0 >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep
%pcre2grep% --exclude "(badpat" abc /dev/null >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep
%pcre2grep% --exclude-from /non/exist abc /dev/null >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep
%pcre2grep% --include-from /non/exist abc /dev/null >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep
%pcre2grep% --file-list=/non/exist abc /dev/null >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 149 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --binary-files=binary "dog" ./testdata/grepbinary & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep
(pushd %srcdir% & %pcre2grep% --binary-files=wrong "dog" ./testdata/grepbinary & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 150 ----------------------------->>testtrygrep
:: The Unix version of this tests checks for whether locales are supported. On Windows,
:: we assume they always are.
set LC_ALL=
set LC_CTYPE=locale.bad
(pushd %srcdir% & %pcre2grep% abc /dev/null & popd) >>testtrygrep 2>&1
echo RC=^%ERRORLEVEL%>>testtrygrep
set LC_CTYPE=

echo ---------------------------- Test 151 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% --colour=always -e this -e The -e "The wo" testdata/grepinputv & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 152 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -nA3 --group-separator="++" "four" ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 153 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -nA3 --no-group-separator "four" ./testdata/grepinputx & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 154 ----------------------------->>testtrygrep
echo. >nul 2>testtemp1grep
(pushd %srcdir% & %pcre2grep% -f %builddir%\testtemp1grep ./testdata/grepinputv & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 155 ----------------------------->>testtrygrep
echo. >testtemp1grep
(pushd %srcdir% & %pcre2grep% -f %builddir%\testtemp1grep ./testdata/grepinputv & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 156 ----------------------------->>testtrygrep
%printf% "\n" >testtemp1grep
(pushd %srcdir% & %pcre2grep% --posix-pattern-file --file %builddir%\testtemp1grep ./testdata/grepinputv & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 157 ----------------------------->>testtrygrep
%printf% "spaces \n" >testtemp1grep
(pushd %srcdir% & %pcre2grep% -o --posix-pattern-file --file=%builddir%\testtemp1grep ./testdata/grepinputv >%builddir%\testtemp2grep && %pcre2grep% -q "s " %builddir%\testtemp2grep & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 158 ----------------------------->>testtrygrep
%printf% "spaces.\n" >testtemp1grep
(pushd %srcdir% & %pcre2grep% -f %builddir%\testtemp1grep ./testdata/grepinputv & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 159 ----------------------------->>testtrygrep
%printf% "spaces.\r\n" >testtemp1grep
(pushd %srcdir% & %pcre2grep% --posix-pattern-file -f%builddir%\testtemp1grep ./testdata/grepinputv & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test 160 ----------------------------->>testtrygrep
(pushd %srcdir% & %pcre2grep% -nC3 "^(ert|jkl)" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep
(pushd %srcdir% & %pcre2grep% -n -B4 -A2 "^(ert|dfg)" ./testdata/grepinput & popd) >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

:: Now compare the results.

%cf% %srcdir%\testdata\grepoutput testtrygrep %cfout%
if ERRORLEVEL 1 exit /b 1


:: These tests require UTF-8 support

if %utf8% neq 0 (
  echo Testing pcre2grep UTF-8 features

  echo ---------------------------- Test U1 ------------------------------>testtrygrep
  (pushd %srcdir% & %pcre2grep% -n -u --newline=any "^X" ./testdata/grepinput8 & popd) >>testtrygrep
  echo RC=^!ERRORLEVEL!>>testtrygrep

  echo ---------------------------- Test U2 ------------------------------>>testtrygrep
  (pushd %srcdir% & %pcre2grep% -n -u -C 3 --newline=any "Match" ./testdata/grepinput8 & popd) >>testtrygrep
  echo RC=^!ERRORLEVEL!>>testtrygrep

  echo ---------------------------- Test U3 ------------------------------>>testtrygrep
  (pushd %srcdir% & %pcre2grep% --line-offsets -u --newline=any --allow-lookaround-bsk "(?<=\K\x{17f})" ./testdata/grepinput8 & popd) >>testtrygrep
  echo RC=^!ERRORLEVEL!>>testtrygrep

  echo ---------------------------- Test U4 ------------------------------>>testtrygrep
  (pushd %srcdir% & %pcre2grep% -u -o "...." ./testdata/grepinputBad8 & popd) >>testtrygrep 2>&1
  echo RC=^!ERRORLEVEL!>>testtrygrep

  echo ---------------------------- Test U5 ------------------------------>>testtrygrep
  (pushd %srcdir% & %pcre2grep% -U -o "...." ./testdata/grepinputBad8 & popd) >>testtrygrep
  echo RC=^!ERRORLEVEL!>>testtrygrep

  echo ---------------------------- Test U6 ----------------------------->>testtrygrep
  (pushd %srcdir% & %pcre2grep% -u -m1 -O "=$x{1d3}$o{744}=" "fox" & popd) <%srcdir%\testdata\grepinputv >>testtrygrep 2>&1
  echo RC=^!ERRORLEVEL!>>testtrygrep

  echo ---------------------------- Test U7 ------------------------------>>testtrygrep
  (pushd %srcdir% & %pcre2grep% -ui --colour=always "k+|\babc\b" ./testdata/grepinput8 & popd) >>testtrygrep
  echo RC=^!ERRORLEVEL!>>testtrygrep

  echo ---------------------------- Test U8 ------------------------------>>testtrygrep
  (pushd %srcdir% & %pcre2grep% -UiEP --colour=always "k+|\babc\b" ./testdata/grepinput8 & popd) >>testtrygrep
  echo RC=^!ERRORLEVEL!>>testtrygrep

  echo ---------------------------- Test U9 ------------------------------>>testtrygrep
  (pushd %srcdir% & %pcre2grep% -u --colour=always "A\d" ./testdata/grepinput8 & popd) >>testtrygrep
  echo RC=^!ERRORLEVEL!>>testtrygrep

  echo ---------------------------- Test U10 ------------------------------>>testtrygrep
  (pushd %srcdir% & %pcre2grep% -u --posix-digit --colour=always "A\d" ./testdata/grepinput8 & popd) >>testtrygrep
  echo RC=^!ERRORLEVEL!>>testtrygrep

  %cf% %srcdir%\testdata\grepoutput8 testtrygrep %cfout%
  if ERRORLEVEL 1 exit /b 1

) else (
  echo Skipping pcre2grep UTF-8 tests: no UTF-8 support in PCRE2 library
)


:: We go to some contortions to try to ensure that the tests for the various
:: newline settings will work in environments where the normal newline sequence
:: is not \n. Do not use exported files, whose line endings might be changed.
:: Instead, create an input file so that its contents are exactly what we want.
:: These tests are run in the build directory.

echo Testing pcre2grep newline settings
%printf% "abc\rdef\r\nghi\njkl" >testNinputgrep

echo ---------------------------- Test N1 ------------------------------>testtrygrep
%pcre2grep% -n -N CR "^(abc|def|ghi|jkl)" testNinputgrep >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep
%pcre2grep% -B1 -n -N CR "^def" testNinputgrep >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test N2 ------------------------------>>testtrygrep
%pcre2grep% -n --newline=crlf "^(abc|def|ghi|jkl)" testNinputgrep >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep
%pcre2grep% -B1 -n -N CRLF "^ghi" testNinputgrep >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test N3 ------------------------------>>testtrygrep
for /f %%a in ('%printf% "def\rjkl"') do set pattern=%%a
%pcre2grep% -n --newline=cr -F "!pattern!" testNinputgrep >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test N4 ------------------------------>>testtrygrep
%pcre2grep% -n --newline=crlf -F -f %srcdir%\testdata\greppatN4 testNinputgrep >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test N5 ------------------------------>>testtrygrep
%pcre2grep% -n --newline=any "^(abc|def|ghi|jkl)" testNinputgrep >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep
%pcre2grep% -B1 -n --newline=any "^def" testNinputgrep >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test N6 ------------------------------>>testtrygrep
%pcre2grep% -n --newline=anycrlf "^(abc|def|ghi|jkl)" testNinputgrep >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep
%pcre2grep% -B1 -n --newline=anycrlf "^jkl" testNinputgrep >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test N7 ------------------------------>>testtrygrep
%printf% "xyz\0abc\0def" >testNinputgrep
%pcre2grep% -na --newline=nul "^(abc|def)" testNinputgrep | %trnull% >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep
%pcre2grep% -B1 -na --newline=nul "^(abc|def)" testNinputgrep | %trnull% >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

echo ---------------------------- Test N8 ------------------------------>>testtrygrep
%pcre2grep% -na --newline=anycrlf "^a" %srcdir%\testdata\grepinputBad8_Trail >>testtrygrep
echo RC=^%ERRORLEVEL%>>testtrygrep

%printf% "\n" >>testtrygrep

%cf% %srcdir%\testdata\grepoutputN testtrygrep %cfout%
if ERRORLEVEL 1 exit /b 1


:: These newline tests need UTF support.

if %utf8% neq 0 (
  echo Testing pcre2grep newline settings with UTF-8 features

  echo ---------------------------- Test UN1 ------------------------------>testtrygrep
  %pcre2grep% -nau --newline=anycrlf "^(abc|def)" %srcdir%\testdata\grepinputUN >>testtrygrep
  echo RC=^!ERRORLEVEL!>>testtrygrep

  echo ---------------------------- Test UN2 ------------------------------>testtrygrep
  %pcre2grep% -nauU --newline=anycrlf "^a" %srcdir%\testdata\grepinputBad8_Trail >>testtrygrep
  echo RC=^!ERRORLEVEL!>>testtrygrep

  %printf% "\n" >>testtrygrep

  %cf% %srcdir%\testdata\grepoutputUN testtrygrep %cfout%
  if ERRORLEVEL 1 exit /b 1

) else (
  echo Skipping pcre2grep newline UTF-8 tests: no UTF-8 support in PCRE2 library
)


:: If pcre2grep supports script callouts, run some tests on them. It is possible
:: to restrict these callouts to the non-fork case, either for security, or for
:: environments that do not support fork(). This is handled by comparing to a
:: different output.

%pcre2grep% --help | %pcre2grep% -q "callout scripts in patterns are supported"
if %ERRORLEVEL% equ 0 (
  echo Testing pcre2grep script callouts

  echo --- Test 1 --->testtrygrep
  %pcre2grep% "(T)(..(.))(?C'cmd|/c echo|Arg1: [$1] [$2] [$3]|Arg2: ^$|${1}^$| ($4) ($14) ($0)')()" %srcdir%\testdata\grepinputv >>testtrygrep
  echo RC=^!ERRORLEVEL!>>testtrygrep
  echo --- Test 2 --->>testtrygrep
  %pcre2grep% "(T)(..(.))()()()()()()()(..)(?C'cmd|/c echo|Arg1: [$11] [${11}]')" %srcdir%\testdata\grepinputv >>testtrygrep
  echo RC=^!ERRORLEVEL!>>testtrygrep
  echo --- Test 3 --->>testtrygrep
  %pcre2grep% "(T)(?C'|$0:$1$n')" %srcdir%\testdata\grepinputv >>testtrygrep
  echo RC=^!ERRORLEVEL!>>testtrygrep
  echo --- Test 4 --->>testtrygrep
  %pcre2grep% "(T)(?C'cscript|//nologo|printf.js|%%s\r\n|$0:$1$n')" %srcdir%\testdata\grepinputv >>testtrygrep
  echo RC=^!ERRORLEVEL!>>testtrygrep
  echo --- Test 5 --->>testtrygrep
  %pcre2grep% "(T)(?C'|$1$n')(*F)" %srcdir%\testdata\grepinputv >>testtrygrep
  echo RC=^!ERRORLEVEL!>>testtrygrep
  echo --- Test 6 --->>testtrygrep
  %pcre2grep% -m1 "(T)(?C'|$0:$1:$x{41}$o{101}$n')" %srcdir%\testdata\grepinputv >>testtrygrep
  echo RC=^!ERRORLEVEL!>>testtrygrep

  %pcre2grep% --help | %pcre2grep% -q "Non-fork callout scripts in patterns are supported"
  if ^!ERRORLEVEL! equ 0 (
    set nonfork=1
    %cf% %srcdir%\testdata\grepoutputCN testtrygrep %cfout%
  ) else (
    set nonfork=0
    %cf% %srcdir%\testdata\grepoutputC testtrygrep %cfout%
  )
  if ERRORLEVEL 1 exit /b 1

  @REM These callout tests need UTF support.

  if %utf8% neq 0 (
    echo Testing pcre2grep script callout with UTF-8 features

    echo --- Test 1 --->testtrygrep
    %pcre2grep% -u "(T)(?C'|$0:$x{a6}$n')" %srcdir%\testdata\grepinputv >>testtrygrep
    echo RC=^!ERRORLEVEL!>>testtrygrep
    echo --- Test 2 --->>testtrygrep
    %pcre2grep% -u "(T)(?C'cscript|//nologo|printf.js|%%s\r\n|$0:$x{a6}$n')" %srcdir%\testdata\grepinputv >>testtrygrep
    echo RC=^!ERRORLEVEL!>>testtrygrep

    if ^!nonfork! equ 1 (
      %cf% %srcdir%\testdata\grepoutputCNU testtrygrep %cfout%
    ) else (
      %cf% %srcdir%\testdata\grepoutputCU testtrygrep %cfout%
    )
    if ERRORLEVEL 1 exit /b 1

  ) else (
    echo Skipping pcre2grep script callout UTF-8 tests: no UTF-8 support in PCRE2 library
  )

) else (
  echo Script callouts are not supported
)


:: Finally, some tests to exercise code that is not tested above, just to be
:: sure that it runs OK. Doing this improves the coverage statistics. The output
:: is not checked.

echo Testing miscellaneous pcre2grep arguments (unchecked)
echo. >nul 2>testtrygrep
call :checkspecial "-xxxxx" 2 || exit /b 1
call :checkspecial "--help" 0 || exit /b 1
call :checkspecial "--line-buffered --colour=auto abc nul" 1 || exit /b 1
call :checkspecial "--line-buffered --color abc nul" 1 || exit /b 1
call :checkspecial "-dskip abc ." 1 || exit /b 1
call :checkspecial "-Dread -Dskip abc nul" 1 || exit /b 1
call :checkspecial "-f %srcdir%\testdata\greplistBad nul" 2 || exit /b 1
call :checkspecial "(unpaired nul" 2 || exit /b 1
call :checkspecial "-e (unpaired1 -e (unpaired2 nul" 2 || exit /b 1


:: Clean up local working files
del testcf printf.js trnull.js testNinputgrep teststderrgrep testtrygrep testtemp1grep testtemp2grep

exit /b 0

:: ------ Function to run and check a special pcre2grep arguments test -------

:checkspecial
  %pcre2grep% %~1 >>testtrygrep 2>&1
  if %ERRORLEVEL% neq %2 (
    echo ** pcre2grep %~1 failed - check testtrygrep
    exit /b 1
  )
  exit /b 0

:: End
