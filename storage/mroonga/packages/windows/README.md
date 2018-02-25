# How to build Windows binaries

## Preparation

TODO...

## Build with Visual C++ Express

You need to use Visual Studio 2015 for Windows Desktop or later to build Mroonga with express
edition. `build-vc2015.bat` is a build batch script to build with
Visual Studio 2015 for Windows Desktop.

Note that you can't build MSI file with Express edition. You need to
use Professional edition or upper editions to build MSI file.

## Build with Visual Studio Community

You can build both zip file MSI file with Professional edition.
But now, this feature is temporary disabled.
If you want to create MSI package, please uncomment in `build-vc2015.bat`.
And then, you can build MSI package with Visual Studio 2015 Community.
