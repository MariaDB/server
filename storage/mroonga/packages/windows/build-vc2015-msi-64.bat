rmdir /S /Q build-vc2015-msi-64
mkdir build-vc2015-msi-64
cd build-vc2015-msi-64
cmake ..\source -G "Visual Studio 14 2015 Win64" > config.log
cmake --build . --config RelWithDebInfo > build.log
cmake --build . --config RelWithDebInfo --target msi > msi.log
move *.msi ..\
cd ..
