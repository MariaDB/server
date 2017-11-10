rmdir /S /Q build-vc2015-msi-32
mkdir build-vc2015-msi-32
cd build-vc2015-msi-32
cmake ..\source -G "Visual Studio 14 2015" > config.log
cmake --build . --config RelWithDebInfo > build.log
cmake --build . --config RelWithDebInfo --target msi > msi.log
move *.msi ..\
cd ..
