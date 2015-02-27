rmdir /S /Q build-vc2013-msi-32
mkdir build-vc2013-msi-32
cd build-vc2013-msi-32
cmake ..\source -G "Visual Studio 12" > config.log
cmake --build . --config RelWithDebInfo > build.log
cmake --build . --config RelWithDebInfo --target msi > msi.log
move *.msi ..\
cd ..
