rmdir /S /Q build-vc2010-msi-32
mkdir build-vc2010-msi-32
cd build-vc2010-msi-32
cmake ..\source -G "Visual Studio 10" > config.log
cmake --build . --config RelWithDebInfo > build.log
cmake --build . --config RelWithDebInfo --target msi > msi.log
move *.msi ..\
cd ..
