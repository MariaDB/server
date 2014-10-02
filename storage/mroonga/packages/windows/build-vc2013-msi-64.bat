rmdir /S /Q build-vc2013-msi-64
mkdir build-vc2013-msi-64
cd build-vc2013-msi-64
cmake ..\source -G "Visual Studio 12 Win64" > config.log
cmake --build . --config RelWithDebInfo > build.log
cmake --build . --config RelWithDebInfo --target msi > msi.log
move *.msi ..\
cd ..
