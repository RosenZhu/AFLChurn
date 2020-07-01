# source
Based on [american fuzzy lop](https://github.com/google/AFL), which is originally developed by Michal Zalewski <lcamtuf@google.com>.

# this project

fuzzing with change-burst info.

## version
Linux 18.04, 64-bit system. 

LLVM 7.0.1

## Install
in afl root dir

    make clean all
    cd llvm_mode
    make clean all

### install libgit2
install OpenSSL (see [troubleshooting](https://github.com/libgit2/libgit2/blob/master/docs/troubleshooting.md)).

    apt-get install libssl-dev

install libgit2

    git clone https://github.com/libgit2/libgit2.git
    mkdir build && cd build
    cmake .. -DCMAKE_INSTALL_PREFIX=/path/to/install/
    cmake --build . --target install

envs:

    export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/path/to/libgit2/lib


compile

    g++ gitexamp.cpp -o gexp -I/path/to/libgit2/include  -L/path/to/libgit2/lib -lgit2

    g++ gitexamp.cpp -o gexp -I/home/xgzhu/apps/libgit2/install/include  -L/home/xgzhu/apps/libgit2/install/lib -lgit2


