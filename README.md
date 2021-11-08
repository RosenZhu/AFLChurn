# AFLChurn: What you change is what you fuzz!

AFLChurn is a regression greybox fuzzer that focusses on code that is changed more recently or more frequently. In our empirical study on bugs in OSSFuzz, we found that every four in five bugs reported in [OSSFuzz](https://github.com/google/oss-fuzz) are introduced by recent changes, so called regressions. Unlike a directed fuzzer, AFLChurn is not directed towards a single recent commit. Instead, it uses the entire commit history of a project to steer the fuzzing efforts towards code regions where such regressions may lurk. For AFLChurn, ever basic block (BB) is a target. However, some BBs have more and others less weight. Specifically, executed BBs that are changed more recently or more frequently will contribute a greater weight towards the power schedule of AFLChurn.

## Project
AFLChurn is developed based on [American Fuzzy Lop (AFL)](https://github.com/google/AFL) which was originally developed by Michal Zalewski <lcamtuf@google.com>. AFLChurn utilizes [git](https://git-scm.com/) to determine how frequently or how recently a BB was changed and an LLVM instrumentation pass to make the compiled program binary efficiently compute the commit-history-based fitness of an executed input.

We tested the code on Linux 18.04, 64-bit system and used Git version 2.33.1.

## Requirements
- Git version >= 2.23: For the option `git blame --ignore-rev`

### upgrade git on linux: >= Git 2.23

```bash
sudo add-apt-repository ppa:git-core/ppa -y
sudo apt-get update
sudo apt-get install git -y
git --version
```

### solution to solve problem of add-apt-repository
It requires the python3.6, but the default is set to python3.8

```bash
sudo python3.6 /usr/bin/apt-add-repository ppa:git-core/ppa -y
```

## Build AFLChurn
To build AFLChurn, execute
```bash
git clone https://github.com/aflchurn/aflchurn.git
cd aflchurn
export AFLCHURN=$PWD
make clean all
cd llvm_mode
make clean all
```

## Instrument target program
### 1. Get distribution of ages/ranks/changes

When cloning your program, **please retain the entire commit history** (i.e., do **not** use `git clone --depth 1 ..`). Currently, we only support `git`.

1.1 To get the distribution, run
```bash
$AFLCHURN/get-distribution -d /path/to/target/program/
```
Run `get-distribution -h` to get other options. 

This will show something like 
```
[+] Threshold of #change: 15
[+] Threshold of age(days): 3418
[+] Threshold of #ranks: 197
```
The corresponding distribution files are in the root folder of the target program, namely `DISTRIBUTION_AGE.txt`, `DISTRIBUTION_CHANGES.txt`, `DISTRIBUTION_RANK.txt`, and `DISTRIBUTION_THRESHOLD.txt`.


1.2 And then, before compiling, set the ENVs based on the result of `get-distribution`
```bash
export AFLCHURN_THRD_CHANGE=15
export AFLCHURN_THRD_AGE=3418
export AFLCHURN_THRD_RANK=197
```
If none of these environment variables are set, default values will apply, which are `AFLCHURN_THRD_CHANGE=10`, `AFLCHURN_THRD_AGE=200`, and `AFLCHURN_THRD_RANK=200`.

### 2. Instrument your Program

When cloning your program, **please retain the entire commit history** (i.e., do **not** use `git clone --depth 1 ..`). Currently, we only support `git`.

Build your project with $AFLCHURN/afl-clang-fast for C code and $AFLCHURN/afl-clang-fast++ for C++ code. For instance,
```bash
CC=$AFLCHURN/afl-clang-fast CXX=$AFLCHURN/afl-clang-fast++ ./configure [...options...]
make
```

## Run AFLChurn on your Program

```bash
afl-fuzz -i <input_dir> -o <out_dir> -- <file_path> [...parameters...]
```

## Configuring AFLChurn
### Fuzzer Options

| Options | args | description | note |
| :---: | :--- | :-------------------------- | :------ |
| `-p` | `anneal` | annealing-based power schedule | default |
| `-p` | `none` | vanilla AFL power schedule | / |
| `-e` | no args | disable ant colony optimisation for byte selection | / |
| `-s` | integer | scale_exponent for power schedule | / |
| `-H` | float | fitness_exponent for power schedule | / |
| `-A` | no args | "increase/decrease" mode for ACO | / |
| `-Z` | no args | alias method for seed selection | experimental |

e.g.,
If `-e` is set, it will not use the ant colony optimization for mutation.

### Environment Variables for our LLVM Instrumentation Pass

| Envs | values | description | note |
| :-------------------- | :--- | :--- | :---- |
| `AFLCHURN_DISABLE_AGE` |   `1`   | disable rdays | / |
| `AFLCHURN_ENABLE_RANK` | `rrank` | enable rrank and disable rdays | / |
| `AFLCHURN_DISABLE_CHURN` | `1` | disable #changes | / |
| `AFLCHURN_INST_RATIO` | integer | select N% BBs to be inserted churn/age | / |
| `AFLCHURN_CHURN_SIG` | `change` | amplify function x | experimental |
| `AFLCHURN_CHURN_SIG` |`change2`| amplify function x^2 | experimental |

e.g., `export AFLCHURN_DISABLE_AGE=1` indicates disabling using days.

# TODO
## Experimental options
### alias method for seed selection
| `-Z` | no args | alias method for seed selection | experimental |

### fuzz all seeds first before using alias method for seed selection
`-D`: if set, fuzz all seeds first = true

## set ENVs for inserting ratio
Two steps to run aflchurn: 1) get distribution; 2) set ENVs for insertion ratio; 3) compile.

| Envs | values | description | note |
| :-------------------- | :--- | :--- | :---- |
| `AFLCHURN_INST_RATIO` | integer | randomly select N% BBs to be inserted churn/age | default: 10|
| `AFLCHURN_THRD_CHANGE`| integer | always insert info when a BB's #change is larger than it | default: 10|
| `AFLCHURN_THRD_AGE`| integer | always insert info when a BB's age (days) is younger than it | default: 200|
| `AFLCHURN_THRD_RANK` | integer |  always insert info when a BB's rank is smaller than it | default: 200|

`default`: If not set, the corresponding variables will be set to default values

