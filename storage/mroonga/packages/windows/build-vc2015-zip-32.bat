rmdir /S /Q build-vc2015-zip-32
mkdir build-vc2015-zip-32
cd build-vc2015-zip-32
cmake ..\source -G "Visual Studio 14 2015" ^
  -DMRN_GROONGA_EMBED=OFF ^
  -DMRN_GROONGA_NORMALIZER_MYSQL_EMBED=OFF ^
  -DGRN_WITH_BUNDLED_LZ4=ON ^
  -DGRN_WITH_BUNDLED_MECAB=ON ^
  > config.log
cmake --build . --config RelWithDebInfo > build.log
cmake --build . --config RelWithDebInfo --target package > zip.log
move *.zip ..\
cd ..
