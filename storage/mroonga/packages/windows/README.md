# How to build Windows binaries

## Preparation

TODO...

## Build with Visual C++ Express

You need to use Visual C++ 2013 or later to build Mroonga with Express
edition. `build-vc2013.bat` is a build batch script to build with
Visual C++ Express 2013.

Note that you can't build MSI file with Express edition. You need to
use Professional edition or upper editions to build MSI file.

## Build with Visual C++ Professional

You can build both zip file MSI file with Professional edition.
But now, this feature is temporary disabled.
If you want to create MSI package, please uncomment in `build-vc2013.bat`.
And then, you can build MSI package with Visual Studio 2013 Professional.
