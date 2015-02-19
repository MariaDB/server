rmdir /S /Q build-vc2010-zip-64
mkdir build-vc2010-zip-64
cd build-vc2010-zip-64
cmake ..\source -G "Visual Studio 10 Win64" -DMRN_GROONGA_EMBED=OFF -DMRN_GROONGA_NORMALIZER_MYSQL_EMBED=OFF > config.log
cmake --build . --config RelWithDebInfo > build.log
cmake --build . --config RelWithDebInfo --target package > zip.log
move *.zip ..\
cd ..
