rmdir /s vs2013_x64
mkdir vs2013_x64
cd vs2013_x64
cmake -G "Visual Studio 12 Win64" -DHDF5_ENABLE_THREADSAFE=ON ..\..
cd ..
pause