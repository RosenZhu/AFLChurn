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

install libgit2 [v1.0.1](https://github-production-release-asset-2e65be.s3.amazonaws.com/901662/67a65980-a649-11ea-8f09-6cbbf461c91b?X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIAIWNJYAX4CSVEH53A%2F20200703%2Fus-east-1%2Fs3%2Faws4_request&X-Amz-Date=20200703T130817Z&X-Amz-Expires=300&X-Amz-Signature=fe97315978eb328ed3963437b79006fd987090d1bd987e6f3fa04d3bfdfba29e&X-Amz-SignedHeaders=host&actor_id=19380991&repo_id=901662&response-content-disposition=attachment%3B%20filename%3Dlibgit2-1.0.1.tar.gz&response-content-type=application%2Foctet-stream)

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
    /home/source/configure [other parameters]

#### Build inside the source directory
Build as normal. Suppose the path of the file "configure" is "/home/source/configure", 

    cd /home/source/
    mkdir build && cd build
    export BURST_LINE_AGE=1
    export BURST_LINE_CHANGE=1
    ../configure [other parameters]

