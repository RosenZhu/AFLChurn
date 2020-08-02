# source
Based on [american fuzzy lop](https://github.com/google/AFL), which is originally developed by Michal Zalewski <lcamtuf@google.com>.

# this project

fuzzing with change-burst info.

## version
Linux 18.04, 64-bit system. 

LLVM 7.0.1

libgit2 v1.0.1

## Install

### install libgit2
install OpenSSL (see [troubleshooting](https://github.com/libgit2/libgit2/blob/master/docs/troubleshooting.md)).

    apt-get install libssl-dev

install libgit2 [v1.0.1](https://github.com/libgit2/libgit2/archive/v1.0.1.tar.gz)

    git clone https://github.com/libgit2/libgit2.git
    mkdir build && cd build
    cmake .. -DCMAKE_INSTALL_PREFIX=/path/to/install/
    cmake --build . --target install

envs:

    export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/path/to/libgit2/lib
    
### install burst
We have two schemes of burst, one is the age of lines and the other is the number of changes of lines. 
We can choose one of the schemes or both of them.

- `export BURST_LINE_AGE=1` enables the age of lines.

- `export BURST_LINE_CHANGE=1` enables the number of changes of lines.

Install

    cd /path/to/root/afl/dir
    make clean all
    cd llvm_mode
    export LIBGIT_INC=/path/to/libgit2/include
    export LIBGIT_LIB=/path/to/libgit2/lib
    make clean all

`LIBGIT_INC` and `LIBGIT_LIB` are used in llvm_mode/Makefile.


### About configure

#### Build outside the source directory
If use configure to generate makefile, and the build directory is not in the source code directory, use absolute path to point configure

For example, if the path of the file "configure" is "/home/source/configure", and the build directory is "/home/mybuild/", then, 

    cd /home/mybuild
    export BURST_LINE_AGE=1
    export BURST_LINE_CHANGE=1
    CC=/path/to/burstfuzz/afl-clang-fast /home/source/configure [...options...]
    make

#### Build inside the source directory
Build as normal. Suppose the path of the file "configure" is "/home/source/configure", 

    cd /home/source/
    mkdir build && cd build
    export BURST_LINE_AGE=1
    export BURST_LINE_CHANGE=1
    CC=/path/to/burstfuzz/afl-clang-fast ../configure [...options...]
    make

Or

    cd /home/source/
    export BURST_LINE_AGE=1
    export BURST_LINE_CHANGE=1
    CC=/path/to/burstfuzz/afl-clang-fast ./configure [...options...]
    make
    
