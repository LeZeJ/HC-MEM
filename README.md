# Building

This project supports [CMake](https://cmake.org/) out of the box.

### Build for POSIX

Quick start:

```bash
leveldb目录：cmake -DCMAKE_BUILD_TYPE=Release .. && cmake --build .
sudo cp libleveldb.a /usr/lib/libleveldb.a
sudo rm -rf /usr/local/include/leveldb/
sudo cp -R ../include/leveldb /usr/local/include/
```

### Building for Windows

First generate the Visual Studio 2017 project/solution files:

```cmd
mkdir build
cd build
cmake -G "Visual Studio 15" ..
```
The default default will build for x86. For 64-bit run:

```cmd
cmake -G "Visual Studio 15 Win64" ..
```

To compile the Windows solution from the command-line:

```cmd
devenv /build Debug leveldb.sln
```

or open leveldb.sln in Visual Studio and build from within.

Please see the CMake documentation and `CMakeLists.txt` for more advanced usage.

### YCSB性能测试

YCSB地址：https://github.com/ls4154/YCSB-cpp

```shell
ycsb目录：make clean
make BIND_LEVELDB=1
rm -rf /tmp/ycsb-leveldb/

./ycsb -load -db leveldb -P workloads/workloada -P leveldb/leveldb.properties -s
./ycsb -run -db leveldb -P workloads/workloada -P leveldb/leveldb.properties -s
or
./run.sh
```

