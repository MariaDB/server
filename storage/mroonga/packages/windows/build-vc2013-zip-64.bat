rmdir /S /Q build-vc2013-zip-64
mkdir build-vc2013-zip-64
cd build-vc2013-zip-64
cmake ..\source -G "Visual Studio 12 Win64" > config.log
cmake --build . --config RelWithDebInfo > build.log
cmake --build . --config RelWithDebInfo --target package > zip.log
move *.zip ..\
cd ..
