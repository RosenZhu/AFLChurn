# source
Based on [american fuzzy lop](https://github.com/google/AFL), which is originally developed by Michal Zalewski <lcamtuf@google.com>.

# this project

fuzzing with change-burst info.

## version
Linux 18.04, 64-bit system. 

LLVM 7.0.1


## Install

   
### install aflchurn
We have two schemes of burst, one is the age of lines and the other is the number of changes of lines. 
We can choose one of the schemes or both of them.

- `export BURST_COMMAND_AGE=1` enables the age of lines using git command.
- `export BURST_COMMAND_CHURN=1` enables the number of changes of lines using git command.

Install

    cd /path/to/root/aflchurn
    make clean all
    cd llvm_mode
    make clean all



### About configure

    export BURST_COMMAND_AGE=1
    export BURST_COMMAND_CHURN=1
    CC=/path/to/aflchurn/afl-clang-fast ./configure [...options...]
    make

Be sure to also include CXX set to afl-clang-fast++ for C++ code.

### configure the time period to record churns

    export BURST_SINCE_MONTHS=num_months

e.g., `export BURST_SINCE_MONTHS=6` indicates recording changes in the recent 6 months

## run fuzzing

    afl-fuzz -i <input_dir> -o <out_dir> -p anneal -e -Z 1 -- <file_path> [...parameters...]

### option -p
power schedule. Default: anneal.

    -p none
    -p anneal
    -p average

### option -b
Choose "age" or "churn". Default: both

    -b none
    -b age
    -b churn

### option -e
Byte score for mutation. 
If `-e` is set, use the ant colony optimisation for mutation.
Otherwise, use the original schemes from AFL.

### option -Z
If `-Z` is set, use alias method to select the next seed based on churns information.

Select the next seed based on the information of exec time or bitmap size.

    -Z 0
    -Z 1
    -Z 2

Meaning:

    enum{
    /* 00 */ ALIAS_TIME,
    /* 01 */ ALIAS_BITMAP,
    /* 02 */ ALIAS_TIME_BITMAP
    };

### option -F
Use `-F` to select how to calculate the fitness score for churn information (how to consider both age and churn).

    -F 0

    enum{
    /* 00 */ FCA_ADD_MAXMIN,
    /* 01 */ FCA_ADD_AVERAGE,
    /* 02 */ FCA_MUL_MAXMIN,
    /* 03 */ FCA_MUL_AVERAGE
    };
