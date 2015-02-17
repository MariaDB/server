rmdir /S /Q build-vc2010-zip-32
mkdir build-vc2010-zip-32
cd build-vc2010-zip-32
cmake ..\source -G "Visual Studio 10" -DMRN_GROONGA_EMBED=OFF -DMRN_GROONGA_NORMALIZER_MYSQL_EMBED=OFF > config.log
cmake --build . --config RelWithDebInfo > build.log
cmake --build . --config RelWithDebInfo --target package > zip.log
move *.zip ..\
cd ..
