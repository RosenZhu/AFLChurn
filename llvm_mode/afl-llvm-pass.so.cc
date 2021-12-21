/*
  Copyright 2015 Google LLC All rights reserved.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at:

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

/*
   american fuzzy lop - LLVM-mode instrumentation pass
   ---------------------------------------------------

   Written by Laszlo Szekeres <lszekeres@google.com> and
              Michal Zalewski <lcamtuf@google.com>

   LLVM integration design comes from Laszlo Szekeres. C bits copied-and-pasted
   from afl-as.c are Michal's fault.

   This library is plugged into LLVM when invoking clang through afl-clang-fast.
   It tells the compiler to add code roughly equivalent to the bits discussed
   in ../afl-as.h.
*/

#define AFL_LLVM_PASS

#include "../config.h"
#include "../debug.h"

//#include <string.h>
#include <set>
#include <map>
#include <cmath>


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <list>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugInfo.h"



using namespace llvm;


namespace {

  class AFLCoverage : public ModulePass {

    public:

      static char ID;

      // unsigned NoSanMetaId;
      // MDTuple *NoneMetaNode;

      AFLCoverage() : ModulePass(ID) { }

      bool runOnModule(Module &M) override;

      // StringRef getPassName() const override {
      //  return "American Fuzzy Lop Instrumentation";
      // }
      

  };

}


char AFLCoverage::ID = 0;


bool startsWith(std::string big_str, std::string small_str){
  if (big_str.compare(0, small_str.length(), small_str) == 0) return true;
  else return false;
}


double inst_norm_age(int max_days, int days_since_last_change){
  double norm_days;
  // if (days_since_last_change < 0) norm_days = 1;
  // else norm_days = 
  //     1 / (log2(days_since_last_change + 2) * log2(days_since_last_change + 2));

  /* Normalize 1/days */
  if (days_since_last_change <= 0 || max_days <= 1) {
    norm_days = 1;
    if (days_since_last_change != 0) WARNF("Current days are less than 0 or maximum days are less than 1.");
  } else if (max_days <= days_since_last_change){
    norm_days = 0;
  } else{
    norm_days = (double)(max_days - days_since_last_change) / 
                            (days_since_last_change * (max_days - 1));
  }

  return norm_days;

}

double inst_norm_rank(int max_rank, int line_rank){
  double norm_ranks;
  // rlogrank
  // if (line_rank < 0) norm_ranks = 1;
  // else norm_ranks = 1 / log2(line_rank + 2);

  // log2rank
  // if (max_rank >= 1){
  //   if (line_rank < 0) norm_ranks = 1;
  //   else norm_ranks = (log2(max_rank + 1) - log2(line_rank + 1)) / log2(max_rank + 1);
  // }

  /* rrank */
  if (line_rank <= 0) {
    norm_ranks = 1;
    WARNF("Rank of lines is less than 0.");
  }
  else norm_ranks = 1 / (double)line_rank;

  return norm_ranks;

}

double inst_norm_change(unsigned int num_changes, unsigned short change_select){
  double norm_chg = 0;

  switch(change_select){
    case CHURN_LOG_CHANGE:
      // logchanges
      if (num_changes < 0) norm_chg = 0;
      else norm_chg = log2(num_changes + 1);
      break;

    case CHURN_CHANGE:
      norm_chg = num_changes;
      break;

    case CHURN_CHANGE2:
      // change^2
      norm_chg = (double)num_changes * num_changes;
      break;
    default:
      FATAL("Wrong CHURN_CHANGE type!");
  }
  // // logchanges
  // if (num_changes < 0) norm_chg = 0;
  // else norm_chg = log2(num_changes + 1);

  // // change^2
  // norm_chg = num_changes * num_changes;
    
  // // xlogchange
  //   if (num_changes < 0) norm_chg = 0;
  //   else norm_chg = (num_changes + 1) * log2(num_changes + 1);

  return norm_chg;

}


/* use popen() to execute git command */
std::string execute_git_cmd (std::string directory, std::string str_cmd){
  FILE *fp;
  std::string str_res = "";
  char ch_git_res[2048];
  std::ostringstream git_cmd;
  git_cmd << "cd " 
          << directory
          << " && "
          << str_cmd;
  fp = popen(git_cmd.str().c_str(), "r");
	if(NULL == fp) return str_res;
	// when cmd fail, output "fatal: ...";
  // when succeed, output result
  if (fscanf(fp, "%s", ch_git_res) == 1) {
    str_res.assign(ch_git_res);  //, strlen(ch_git_res)
  }

  if (startsWith(str_res, "fatal")){
    str_res = "";
  }

  pclose(fp);

  return str_res;

}

/* Check if file exists in HEAD using command mode.
return:
    exist: 1; not exist: 0 */
bool is_file_exist(std::string relative_file_path, std::string git_directory,
                      std::string commit_sha){

  //string cmd("cd /home/usrname/repo && git cat-file -e HEAD:util/read.h 2>&1");
  std::ostringstream cmd;

  char result_buf[1024];
  bool isSuccess = false;
  FILE *fp;

  if(access(git_directory.c_str(), F_OK) == -1) return false;
  
  cmd << "cd " << git_directory << " && git cat-file -e "
      << commit_sha << ":" 
      << relative_file_path << " 2>&1";

	fp = popen(cmd.str().c_str(), "r");
	if(NULL == fp) return false;
	// when cmd fail, output "fatal: Path 'tdio.h' does not exist in 'HEAD'";
  // when succeed, output nothing
  if (fgets(result_buf, sizeof(result_buf), fp) != NULL) isSuccess = false;
  else isSuccess = true;
	
  pclose(fp);
  
  return isSuccess;

}


/* if return value is WRONG_VALUE, something wrong happens */
int get_commit_time_days(std::string directory, std::string git_cmd){
  unsigned long unix_time = 0;
  FILE *dfp;
  std::ostringstream datecmd;

  datecmd << "cd " << directory
          << " && "
          << git_cmd;
  dfp = popen(datecmd.str().c_str(), "r");

  if (NULL == dfp) return WRONG_VALUE;
  if (fscanf(dfp, "%lu", &unix_time) != 1){
    pclose(dfp);
    return WRONG_VALUE;
  } 

  pclose(dfp);

  return unix_time / 86400;

}

/* Get the number of commits before HEAD;
  if return value is WRONG_VALUE, something wrong happens */
int get_max_ranks(std::string git_directory){

  FILE *dfp;
  unsigned int head_num_parents;
  std::ostringstream headcmd;
  headcmd << "cd " << git_directory
          << " && git rev-list --count HEAD";
  dfp = popen(headcmd.str().c_str(), "r");
  if (NULL == dfp) return WRONG_VALUE;
  if (fscanf(dfp, "%u", &head_num_parents) != 1){
    pclose(dfp);
    return WRONG_VALUE;
  } 

  pclose(dfp);

  return head_num_parents;

}


/* if same, return true */
bool isCommitsSame(std::string cmt1, std::string cmt2){
  if (cmt1.length() <= cmt2.length())
    return cmt1.compare(0, cmt1.length(), cmt2, 0, cmt1.length()) == 0;

  return cmt2.compare(0, cmt2.length(), cmt1, 0, cmt2.length()) == 0;
  
}


// get unix time of a commit
unsigned long cmt_unix_time(std::string git_directory, std::string cmt_sha){
    FILE *unixfp;
    unsigned long utime = 0;
    std::ostringstream unix_cmd;
    // result: 1557509398
    unix_cmd << "cd " << git_directory 
            << " && git show --no-patch --no-notes --pretty='%at' " 
            << cmt_sha;
    unixfp = popen(unix_cmd.str().c_str(), "r");
    if (NULL== unixfp) return 0;
    if (fscanf(unixfp, "%lu", &utime) != 1) utime = 0;
    pclose(unixfp);

    return utime;

}


/* git diff parent HEAD */
/* git diff, get changed lines in current commit.
    Find the changed line numbers in file relative_file_path as it was changed in HEAD, 
    and add them to the list changed_lines_cur_commit     
 */
void git_diff_parent_head(std::string git_directory, std::string relative_file_path, 
              std::string parent_commit, std::set<unsigned int> &changed_lines_cur_commit){

    std::ostringstream cmd;
    
    std::string current_line_range;
    FILE *fp;
    int num_start, num_count;

    // git show: parent_commit(-) current_commit(+)
    // result: "@@ -8,0 +9,2 @@" or "@@ -10 +11,0 @@" or "@@ -466,8 +475 @@" or "@@ -8 +9 @@"
    // output: "9,2" or "11,0" (the lines are deleted) or "8"
    cmd << "cd " << git_directory 
        << " && git diff --oneline -U0 " 
        << parent_commit << " HEAD" 
        << " -- " << relative_file_path
        << " | grep -o -P \"^@@ -[0-9]+(,[0-9]+)? \\+\\K[0-9]+(,[0-9]+)?(?= @@)\"";

    fp = popen(cmd.str().c_str(), "r");
    if(NULL == fp) return;
    // get numbers in (+): current commit
    num_count = -1; // the result shows no count
    while(fscanf(fp, "%d,%d", &num_start, &num_count) >= 1){
      if (num_count == -1){ // result: 8
        changed_lines_cur_commit.insert(num_start);
      } else if (num_count > 0){ // result: 9,2
        for (int i = 0; i < num_count; i++){
          changed_lines_cur_commit.insert(num_start + i);
        }
      }

      num_count = -1;
    }

   pclose(fp);
}

/* Get changes in HEAD commit.
  git diff HEAD to each parent commit.    
 */
void getHeadChanges(std::string git_directory, std::string relative_file_path, 
                                std::set<unsigned int> &changed_lines_num){
    std::string commit_sha;
    char res_commit[SHA_LENGTH];
    FILE *fp;
    std::ostringstream pc_cmd;

    //get parent commits of HEAD
    // succeeds: output parent commit sha; fail: output "fatal: ..."
    pc_cmd << "cd " << git_directory << " && "
           << "git show -s --pretty=%P HEAD 2>&1";
    fp = popen(pc_cmd.str().c_str(), "r");
    if (NULL == fp) return;
    while (fscanf(fp, "%s", res_commit) == 1){
      commit_sha.assign(res_commit);
      if (commit_sha.empty()) break;
      if (startsWith(commit_sha, "fatal")) break;

      git_diff_parent_head(git_directory, relative_file_path, 
                                  commit_sha, changed_lines_num);
    }

    pclose(fp);

}

/* get 'git blame' in HEAD commit */
  /* largest unix time => SHA of commit with the largest unix time => line pair 
  //unix time, blame commit sha
  map<unsigned long, string> utime2commit; 
  // SHA of blame commit, (blame line number, head line number)
  map<string, map<unsigned int, unsigned int>> commit2Blame2HeadLine; */
void createHeadRecords(std::string relative_file_path, std::string git_directory,
              std::map<unsigned long, std::string> &utime2commit, 
              std::map<std::string, std::map<unsigned int, unsigned int>> &commit2Blame2HeadLine){
    /* git blame -p $commit_SHA  -- relative_file_path
       | grep -Po "^[0-9a-f]+ [0-9]+ [0-9]+" */
  /* result: output: commit_hash old_line_num current_line_num
                      7db7d5b73368 12 31 */

  char blame_commit[SHA_LENGTH] = {0};
  unsigned int head_line, blame_line;
  unsigned long utime;
  FILE *headfp;

  std::string str_blame_commit;
  std::map<std::string, unsigned long> cmt2time;

  std::ostringstream headblame_cmd;
  headblame_cmd << "cd " << git_directory
            << " && git blame -p HEAD -- " << relative_file_path
            << " | grep -Po \"^[0-9a-f]+ [0-9]+ [0-9]+\"";
  headfp = popen(headblame_cmd.str().c_str(), "r");
  if (NULL == headfp) return;

  while (fscanf(headfp,"%s %u %u", blame_commit, &blame_line, &head_line) == 3){
    str_blame_commit.assign(blame_commit);
    if (str_blame_commit.empty()) continue;

    if (!cmt2time.count(str_blame_commit)){
      utime = cmt_unix_time(git_directory, str_blame_commit);
      cmt2time[str_blame_commit] = utime;
      utime2commit[utime] = str_blame_commit;
    } else{
      utime2commit[cmt2time[str_blame_commit]] = str_blame_commit;
    }

    commit2Blame2HeadLine[str_blame_commit][blame_line] = head_line;
    
  }

  pclose(headfp);
      

}

/* Count lines in the latest commit. 
    We travel backward time so that commits will not be "git blame"d repeatedly. 
    utime_largest: the unix time of the most recent blamed commit */
void countInLatestCommit(std::string relative_file_path, std::string git_directory,
              std::map<unsigned long, std::string> &utime2commit,
              unsigned long utime_largest, 
              std::map<std::string, std::map<unsigned int, unsigned int>> &commit2Blame2HeadLine,
              std::map<unsigned int, unsigned int> &headline2count){

  char blamed_commit[SHA_LENGTH] = {0};
  unsigned int cur_line, blame_line, head_line;
  unsigned long utime;
  FILE *headfp;

  std::string str_blame_commit, cur_commit;
  std::map<std::string, unsigned long> cmt2time;
  std::ostringstream headblame_cmd;

  cur_commit = utime2commit[utime_largest];

  /* Get the lines touched in the cur_commit: previous blame line, head line;
      In the current commit, the previous blamed line == the current line */
  auto cur2headLines = commit2Blame2HeadLine[cur_commit];
  auto curline_min = cur2headLines.begin()->first;
  auto curline_max = cur2headLines.rbegin()->first;

  // check if file exists in current commit. TODO: renamed file?
  if (!is_file_exist(relative_file_path, git_directory, cur_commit)) return;

  /* git blame -p -L 10,30 1485aacb161d72  -- meson.build 
        | grep -Po "^[0-9a-f]+ [0-9]+ [0-9]+" */
  /* output: blame_commit_hash old_line_num current_line_num
                      7db7d5b73368 12 31 */
  headblame_cmd << "cd " << git_directory
            << " && git blame -p "    // -p: for the "grep -Po"
            << " -L "<< curline_min << "," << curline_max << " "  // line range
            << cur_commit  // the current commit
            << " --ignore-rev " << cur_commit // Don't blame itself!!!
            << " -- " << relative_file_path
            << " | grep -Po \"^[0-9a-f]+ [0-9]+ [0-9]+\"";
  headfp = popen(headblame_cmd.str().c_str(), "r");
  if (NULL == headfp) return;
  // head line => cur_line => blame line
  while (fscanf(headfp, "%s %u %u", blamed_commit, &blame_line, &cur_line) == 3){

    str_blame_commit.assign(blamed_commit);
    if (str_blame_commit.empty()) continue;
    // only deal with the lines touched in the current commit (i.e., previous blamed commit)
    if (!cur2headLines.count(cur_line)) continue;

    /* If the blamed commit equals to the current commit, 
          the current commit is the one that creates the lines */
    if (isCommitsSame(str_blame_commit, cur_commit)) continue;
    
    // don't repeatedly compute the unix time of the same commit
    if (!cmt2time.count(str_blame_commit)){
      utime = cmt_unix_time(git_directory, str_blame_commit);
      cmt2time[str_blame_commit] = utime;
      utime2commit[utime] = str_blame_commit;
    } else{
      utime2commit[cmt2time[str_blame_commit]] = str_blame_commit;
    }

    head_line = cur2headLines[cur_line];
    commit2Blame2HeadLine[str_blame_commit][blame_line] = head_line;

    /* Finally!! Count the #changes */
    if (headline2count.count(head_line))
        headline2count[head_line]++;
    else
        headline2count[head_line] = 1;

  }

  pclose(headfp);

}

/* get line #changes using git blame */
void calculate_line_change_git_blame(std::string relative_file_path, std::string git_directory,
                    std::map<std::string, std::map<unsigned int, double>> &file2line2change_map,
                    unsigned short change_sig){
  unsigned long utime_largest;
  std::string cur_commit;
   /* largest unix time => SHA of commit with the largest unix time => line pair */
  // unix time, blamed commit sha
  std::map<unsigned long, std::string> utime2commit;
  // SHA of blamed commit, (line number of blamed commit, head line number)
  std::map<std::string, std::map<unsigned int, unsigned int>> commit2Blame2HeadLine;
  // head line, count of #changes
  std::map<unsigned int, unsigned int> headline2count;

  /* get 'git blame' in HEAD commit */
  createHeadRecords(relative_file_path, git_directory, 
                          utime2commit, commit2Blame2HeadLine);

  /* Get the #changes of lines.
  Always analyze the commit with the largest unix time (the most recent date) */
  while(!utime2commit.empty()){
    // get the commit with the largest unix time
    auto largest_utime_it = utime2commit.rbegin();
    utime_largest = largest_utime_it->first;
    cur_commit = largest_utime_it->second;

    countInLatestCommit(relative_file_path, git_directory, utime2commit, utime_largest, 
                              commit2Blame2HeadLine, headline2count);

    /* remove the record of the analyzed commit */
    commit2Blame2HeadLine.erase(cur_commit);
    utime2commit.erase(utime_largest);
  }

  // Changes in HEAD
  std::set<unsigned int> changed_lines_num;
  getHeadChanges(git_directory, relative_file_path, changed_lines_num);
  if (!changed_lines_num.empty()){
    for (auto headcn : changed_lines_num){
      if (headline2count.count(headcn)) headline2count[headcn]++;
      else headline2count[headcn] = 1;
    }
  }

  /* Get changes */
  std::map <unsigned int, double> tmp_line2changes;
  if (!headline2count.empty()){
    for (auto h2c: headline2count){
      tmp_line2changes[h2c.first] = inst_norm_change(h2c.second, change_sig);
    }

    file2line2change_map[relative_file_path] = tmp_line2changes;
  }


}


/* get age of lines using git command line. 
  git_directory: /home/usrname/repo/
  head_commit_days: unix time of head commit, in days;
  init_commit_days: unix time of initial 
*/
bool calculate_line_age(std::string relative_file_path, std::string git_directory,
                    std::map<std::string, std::map<unsigned int, double>> &file2line2age_map,
                    int head_commit_days, int init_commit_days){

  std::map<unsigned int, double> line_age_days;

  /*
  getting pairs [unix_time line_number]
  // cd ${git_directory} &&
  // git blame --date=unix ${relative_file_path} \
  // | grep -o -P "[0-9]{9}[0-9]? [0-9]+"
  */

  std::ostringstream cmd;
  FILE *fp;
  unsigned long unix_time;
  unsigned int line;
  int days_since_last_change;

  if (head_commit_days==WRONG_VALUE || init_commit_days==WRONG_VALUE) return false;

  int max_days = head_commit_days - init_commit_days;

  cmd << "cd " << git_directory << " && git blame --date=unix " << relative_file_path
        << " | grep -o -P \"[0-9]{9}[0-9]? +[0-9]+\"";

  fp = popen(cmd.str().c_str(), "r");
  if(NULL == fp) return false;
  // get line by line
  while(fscanf(fp, "%lu %u", &unix_time, &line) == 2){
    days_since_last_change = head_commit_days - unix_time / 86400; //days

    line_age_days[line] = inst_norm_age(max_days, days_since_last_change);
    
  }

  if (!line_age_days.empty())
      file2line2age_map[relative_file_path] = line_age_days;

  pclose(fp);

  return true;

}

/* get rank of line ages.
  rank = (the number of commits until HEAD) - (the number of commits until commit A);
 */
bool cal_line_age_rank(std::string relative_file_path, std::string git_directory,
                std::map<std::string, std::map<unsigned int, double>> &file2line2rank_map,
                std::map<std::string, double> &commit2rank,
                int head_num_parents){

  char line_commit_sha[256];
  FILE *dfp, *curdfp;
  unsigned int nothing_line, line_num;
  std::map<unsigned int, double> line_rank;
  unsigned int cur_num_parents;
  int rank4line;

  if (head_num_parents == WRONG_VALUE) return false;

  /* output: commit_hash old_line_num current_line_num
        e.g., 9f1a353f68d6586b898c47c71a7631cdc816215f 167 346
   */
  std::ostringstream blamecmd;
  blamecmd << "cd " << git_directory
        << " && git blame -p -- " << relative_file_path
        << " | grep -o \"^[0-9a-f]* [0-9]* [0-9]*\"";

  dfp = popen(blamecmd.str().c_str(), "r");
  if(NULL == dfp) return false;

  std::ostringstream rankcmd;
  while (fscanf (dfp, "%s %u %u", line_commit_sha, &nothing_line, &line_num) == 3){
    std::string str_cmt(line_commit_sha);
    if (commit2rank.count(str_cmt)){
      line_rank[line_num] = commit2rank[str_cmt];
    } else {
      rankcmd.str("");
      rankcmd.clear();
      rankcmd << "cd " << git_directory
              << " && git rev-list --count "
              << line_commit_sha;
      curdfp = popen(rankcmd.str().c_str(), "r");
      if(NULL == curdfp) continue;
      if (fscanf (curdfp, "%u", &cur_num_parents) == 1){
        rank4line = head_num_parents - cur_num_parents;
        commit2rank[str_cmt] = line_rank[line_num] 
                             = inst_norm_rank(head_num_parents, rank4line);
      }
      pclose(curdfp);
    }
    
  }
  pclose(dfp);

  if (!line_rank.empty()) file2line2rank_map[relative_file_path] = line_rank;
  return true;
  
}


/* Change the filename to relative path (relative to souce dir) without "../" or "./" in the path.
Input:
  relative_file_path: relative path of source files, relative to base_directory
  base_directory: absolute path of directories in building directory
  git_directory: absolute path of git repo directory (root of source code)
Output:
  clean relative path of a file
 */
std::string get_file_path_relative_to_git_dir(std::string relative_file_path, 
                    std::string base_directory, std::string git_directory){

    std::string clean_relative_path;

  
  if (startsWith(relative_file_path, "/")){
    // "/path/to/configure": relative_file_path = /path/to/file.c
    // remove substring, which is the same as git_directory, from relative_file_path
    relative_file_path.erase(0, git_directory.length());  // relative path
    clean_relative_path = relative_file_path;
  } else{
    // "../configure" or "./configure"
    // relative_file_path could be src/file.c, build/../src/file.c, or src/./file.c
    // relative_file_path is relative to base_directory here
    base_directory.append("/");
    base_directory.append(relative_file_path);
    // remove "../" or "./"
    char* resolved_path = realpath(base_directory.c_str(), NULL);
    //TODO: why is it NULL?
    if (resolved_path == NULL) clean_relative_path = "";
    else{
      clean_relative_path.append(resolved_path);

      free(resolved_path);

      clean_relative_path.erase(0, git_directory.length());  // relative path
    }  
  }

  return clean_relative_path;

}




bool AFLCoverage::runOnModule(Module &M) {

  LLVMContext &C = M.getContext();
  
  Type *VoidTy;
  IntegerType *Int1Ty;
  IntegerType *Int8Ty;
  IntegerType *Int32Ty;
  IntegerType *Int64Ty;
  Type *Int8PtrTy;
  Type *Int64PtrTy;
  Type *DoubleTy;
  Type *FloatTy;
  GlobalVariable *AFLMapPtr;
  GlobalVariable *AFLPrevLoc;
  unsigned NoSanMetaId;
  MDTuple *NoneMetaNode;
  VoidTy = Type::getVoidTy(C);
  Int1Ty = IntegerType::getInt1Ty(C);
  Int8Ty = IntegerType::getInt8Ty(C);
  Int32Ty = IntegerType::getInt32Ty(C);
  Int64Ty = IntegerType::getInt64Ty(C);
  Int8PtrTy = PointerType::getUnqual(Int8Ty);
  Int64PtrTy = PointerType::getUnqual(Int64Ty);
  DoubleTy = Type::getDoubleTy(C);
  FloatTy = Type::getFloatTy(C);
  NoSanMetaId = C.getMDKindID("nosanitize");
  NoneMetaNode = MDNode::get(C, None);

  /* Show a banner */

  char be_quiet = 0;

  if (isatty(2) && !getenv("AFL_QUIET")) {

    SAYF(cCYA "afl-llvm-pass " cBRI VERSION cRST " by <aflchurn>\n");

  } else be_quiet = 1;

  /* Decide instrumentation ratio */

  char* inst_ratio_str = getenv("AFL_INST_RATIO");
  unsigned int inst_ratio = 100;

  if (inst_ratio_str) {

    if (sscanf(inst_ratio_str, "%u", &inst_ratio) != 1 || !inst_ratio ||
        inst_ratio > 100)
      FATAL("Bad value of AFL_INST_RATIO (must be between 1 and 100)");

  }

  // default: instrument changes and days
  bool use_cmd_change = true, use_cmd_age_rank = false, use_cmd_age = true;

  char *day_sig_str, *change_sig_str;

  unsigned short change_sig = CHURN_LOG_CHANGE; //day_sig, 

  if (getenv("AFLCHURN_DISABLE_AGE")) use_cmd_age = false;
  day_sig_str = getenv("AFLCHURN_ENABLE_RANK");
  if (day_sig_str){
    if (!strcmp(day_sig_str, "rrank")){
      use_cmd_age_rank = true;
      use_cmd_age = false;
    } else{
      FATAL("Set proper age signal");
    }
  }

  if (getenv("AFLCHURN_DISABLE_CHURN")) use_cmd_change = false;
  change_sig_str = getenv("AFLCHURN_CHURN_SIG");
  if (change_sig_str){
    if (!use_cmd_change) FATAL("Cannot simultaneously set AFLCHURN_DISABLE_CHURN and AFLCHURN_CHURN_SIG!");
    if (!strcmp(change_sig_str, "logchange")){
      change_sig = CHURN_LOG_CHANGE;
    } else if (!strcmp(change_sig_str, "change")){
      change_sig = CHURN_CHANGE;
    } else if (!strcmp(change_sig_str, "change2")){
      change_sig = CHURN_CHANGE2;
    } else {
      FATAL("Wrong change signal.");
    }
  }

  /* ratio for randomly selected BBs */
  unsigned int bb_select_ratio = CHURN_INSERT_RATIO;
  char *bb_select_ratio_str = getenv("AFLCHURN_INST_RATIO");

  if (bb_select_ratio_str) {
    if (sscanf(bb_select_ratio_str, "%u", &bb_select_ratio) != 1 || !bb_select_ratio ||
        bb_select_ratio > 100)
      FATAL("Bad value of AFLCHURN_INST_RATIO (must be between 1 and 100)");
  }

  // Choose part of BBs to insert the age/change signal
  unsigned int changes_inst_thred = THRESHOLD_CHANGES_DEFAULT,
  age_inst_thred = THRESHOLD_DAYS_DEFAULT, 
  rank_inst_thred = THRESHOLD_RANKS_DEFAULT;

  char *str_changes_inst_thred = getenv("AFLCHURN_THRD_CHANGE");
  if (str_changes_inst_thred){
    if (sscanf(str_changes_inst_thred, "%u", &changes_inst_thred) != 1)
      FATAL("Bad value of AFLCHURN_THRD_CHANGE (must be larger than 0)");
  }
  
  char *str_age_inst_thred = getenv("AFLCHURN_THRD_AGE");
  if (str_age_inst_thred){
    if (sscanf(str_age_inst_thred, "%u", &age_inst_thred) != 1)
      FATAL("Bad value of AFLCHURN_THRD_AGE (must be larger than 0)");
  }
  
  char *str_rank_inst_thred = getenv("AFLCHURN_THRD_RANK");
  if (str_rank_inst_thred){
    if (sscanf(str_rank_inst_thred, "%u", &rank_inst_thred) != 1)
      FATAL("Bad value of AFLCHURN_THRD_RANK (must be larger than 0)");
  }

  /* Get globals for the SHM region and the previous location. Note that
     __afl_prev_loc is thread-local. */

  AFLMapPtr =
      new GlobalVariable(M, PointerType::get(Int8Ty, 0), false,
                         GlobalValue::ExternalLinkage, 0, "__afl_area_ptr");

  AFLPrevLoc = new GlobalVariable(
      M, Int32Ty, false, GlobalValue::ExternalLinkage, 0, "__afl_prev_loc",
      0, GlobalVariable::GeneralDynamicTLSModel, 0, false);

  /* Instrument all the things! */

  int inst_blocks = 0, inst_ages = 0, inst_changes = 0, inst_fitness = 0;
  double module_total_ages = 0, module_total_changes = 0, module_total_fitness = 0,
      module_ave_ages = 0, module_ave_chanegs = 0, module_ave_fitness = 0;

  int init_commit_days = 0, head_commit_days = 0; // for age
  int head_num_parents = 0; // for ranks
  double norm_change_thd = 0, norm_age_thd = 0, norm_rank_thd = 0;

  std::set<unsigned int> bb_lines;
  std::set<std::string> unexist_files, processed_files;
  unsigned int line;
  std::string git_path;
  
  int git_no_found = 1, // 0: found; otherwise, not found
      is_one_commit = 0; // don't calculate for --depth 1

  std::map<std::string, double> commit_rank;
  // file name (relative path): line NO. , score
  std::map<std::string, std::map<unsigned int, double>> map_age_scores, map_bursts_scores, map_rank_age;

  for (auto &F : M){
    /* Get repository path and object */
    if (git_no_found && !is_one_commit){
      SmallVector<std::pair<unsigned, MDNode *>, 4> MDs;
      std::string funcdir, funcfile, func_abs_path;// func_clean_path;
      
      F.getAllMetadata(MDs);
      for (auto &MD : MDs) {
        if (MDNode *N = MD.second) {
          if (auto *subProgram = dyn_cast<DISubprogram>(N)) {
            funcfile = subProgram->getFilename().str();
            funcdir = subProgram->getDirectory().str();

            if (!funcfile.empty() && !funcdir.empty()){
              // fix path here; if "funcfile" does not start with "/", use funcdir as the prefix of funcfile
              if (!startsWith(funcfile, "/")){
                func_abs_path = funcdir;
                func_abs_path.append("/");
                func_abs_path.append(funcfile);
              } else func_abs_path = funcfile;

              // get the real path for the current file
              char *realp = realpath(func_abs_path.c_str(), NULL);
              if (realp == NULL) git_no_found = 1;
              else{
                func_abs_path.assign(realp);
                free(realp);
                git_no_found = 0;
              }

              if (!git_no_found){
                /* Directory of the file. */
                func_abs_path = func_abs_path.substr(0, func_abs_path.find_last_of("\\/")); //remove filename in string
                //git rev-parse --show-toplevel: show the root folder of a repository
                // result: /home/usr/repo_name
                std::string cmd_repo ("git rev-parse --show-toplevel");
                
                git_path = execute_git_cmd(func_abs_path, cmd_repo);
                if (git_path.empty()) git_no_found = 1;
                else git_path.append("/"); // result: /home/usr/repo_name/
                
                /* Check shallow git repository */
                // git rev-list HEAD --count: count the number of commits
                if (!git_no_found){
                  std::string cmd_count ("git rev-list HEAD --count");
                  std::string commit_cnt = execute_git_cmd(git_path, cmd_count);
                  
                  if (commit_cnt.compare("1") == 0){ //only one commit
                    git_no_found = 1;
                    is_one_commit = 1;
                    OKF("Shallow repository clone. Ignoring file %s.", funcfile.c_str());
                    break;
                  }
                  // #change threshold
                  //get commit time
                  std::string head_cmd("git show -s --format=%ct HEAD");
                  head_commit_days = get_commit_time_days(git_path, head_cmd);
                  std::string init_cmd("git log --reverse --date=unix --oneline --format=%cd | head -n1");
                  init_commit_days = get_commit_time_days(git_path, init_cmd);
                  /* Get the number of commits before HEAD */
                  head_num_parents = get_max_ranks(git_path);
                  /* thresholds */
                  norm_change_thd = inst_norm_change(changes_inst_thred, change_sig);
                  norm_age_thd = inst_norm_age(head_commit_days - init_commit_days, age_inst_thred);
                  norm_rank_thd = inst_norm_rank(head_num_parents, rank_inst_thred);

                  // std::cout << "changes threshold: "<< changes_inst_thred
                  //         << "; head days: " << head_commit_days
                  //         << "; init days: " << init_commit_days
                  //         << "; head's parents: "<< head_num_parents
                  //         << std::endl;
                  break;
                }
                
              }
          
            }
              
          }
        }
      }
    }
    
    for (auto &BB : F) {
      
      BasicBlock::iterator IP = BB.getFirstInsertionPt();
      IRBuilder<> IRB(&(*IP));

      if (AFL_R(100) >= inst_ratio) continue;

      /* Make up cur_loc */

      unsigned int cur_loc = AFL_R(MAP_SIZE);

      ConstantInt *CurLoc = ConstantInt::get(Int32Ty, cur_loc);

      double bb_rank_age = 0, bb_age_best = 0, bb_burst_best = 0, bb_rank_best = 0;
      double bb_raw_fitness = 0, tmp_score = 0;
      bool bb_raw_fitness_flag = false;
      
      if (!bb_lines.empty())
            bb_lines.clear();
      bb_lines.insert(0);
      
      for (auto &I: BB){
  
        line = 0;
        std::string filename, filedir, clean_relative_path;
        /* Connect targets with instructions */
        DILocation *Loc = I.getDebugLoc().get(); 
        if (Loc && !git_no_found){
          filename = Loc->getFilename().str();
          filedir = Loc->getDirectory().str();
          line = Loc->getLine();
          if (filename.empty()){
            DILocation *oDILoc = Loc->getInlinedAt();
            if (oDILoc){
              line = oDILoc->getLine();
              filename = oDILoc->getFilename().str();
              filedir = oDILoc->getDirectory().str();
            }
          }

          /* take care of git blame path: relative to repo dir */
          if (!filename.empty() && !filedir.empty()){
            // std::cout << "file name: " << filename << std::endl << "file dir: " << filedir <<std::endl;
            clean_relative_path = get_file_path_relative_to_git_dir(filename, filedir, git_path);
            // std::cout << "relative path: " << clean_relative_path << std::endl;
            if (!clean_relative_path.empty()){
              /* calculate score of a block */
                /* Check if file exists in HEAD using command mode */
              if (unexist_files.count(clean_relative_path)) break;

              if (!bb_lines.count(line)){
                bb_lines.insert(line);
                /* process files that have not been processed */
                if (!processed_files.count(clean_relative_path)){
                  processed_files.insert(clean_relative_path);

                  /* Check if file exists in HEAD using command mode */
                  if (!is_file_exist(clean_relative_path, git_path, "HEAD")){
                    unexist_files.insert(clean_relative_path);
                    break;
                  }
                  
                  /* the ages for lines */
                  if (use_cmd_age) {
                    calculate_line_age(clean_relative_path, git_path, map_age_scores,
                                                head_commit_days, init_commit_days);
                  }
                  if (use_cmd_age_rank){
                    cal_line_age_rank(clean_relative_path, git_path, map_rank_age, 
                                            commit_rank, head_num_parents);
                  }
                  /* the number of changes for lines */
                  if (use_cmd_change){
                    calculate_line_change_git_blame(clean_relative_path, git_path, 
                                                      map_bursts_scores, change_sig);
                  }
                  
                }
                
                if (use_cmd_age){
                  // calculate line age
                  if (map_age_scores.count(clean_relative_path)){
                    if (map_age_scores[clean_relative_path].count(line)){
                      // use the best value of a line as the value of a BB
                      tmp_score = map_age_scores[clean_relative_path][line];
                      if (bb_age_best < tmp_score) bb_rank_age = bb_age_best = tmp_score;
                    }
                  }
                }

                if (use_cmd_age_rank){
                  if (map_rank_age.count(clean_relative_path)){
                    if (map_rank_age[clean_relative_path].count(line)){
                      tmp_score = map_rank_age[clean_relative_path][line];
                      if (bb_rank_best < tmp_score) bb_rank_age = bb_rank_best = tmp_score;
                    }
                  }
                }

                if (use_cmd_change){
                  // calculate line change
                  if (map_bursts_scores.count(clean_relative_path)){
                    if (map_bursts_scores[clean_relative_path].count(line)){
                      tmp_score = map_bursts_scores[clean_relative_path][line];
                      if (bb_burst_best < tmp_score) bb_burst_best = tmp_score;
                    }
                  }
                }
              
              }
            }
          

          }
        }
      } 
 
      /* Load prev_loc */

      LoadInst *PrevLoc = IRB.CreateLoad(AFLPrevLoc);
      PrevLoc->setMetadata(NoSanMetaId, NoneMetaNode);
      Value *PrevLocCasted = IRB.CreateZExt(PrevLoc, IRB.getInt32Ty());

      /* Load SHM pointer */
      
      LoadInst *MapPtr = IRB.CreateLoad(AFLMapPtr);
      MapPtr->setMetadata(NoSanMetaId, NoneMetaNode);
      Value *MapPtrIdx =
          IRB.CreateGEP(MapPtr, IRB.CreateXor(PrevLocCasted, CurLoc));

      /* Update bitmap */

      LoadInst *Counter = IRB.CreateLoad(MapPtrIdx);
      Counter->setMetadata(NoSanMetaId, NoneMetaNode);
      Value *Incr = IRB.CreateAdd(Counter, ConstantInt::get(Int8Ty, 1));
      IRB.CreateStore(Incr, MapPtrIdx)
          ->setMetadata(NoSanMetaId, NoneMetaNode);


      /* Set prev_loc to cur_loc >> 1 */

      StoreInst *Store =
          IRB.CreateStore(ConstantInt::get(Int32Ty, cur_loc >> 1), AFLPrevLoc);
      Store->setMetadata(NoSanMetaId, NoneMetaNode);

      /* insert age/churn into BBs */
      if ((use_cmd_age || use_cmd_age_rank) && !use_cmd_change){
        /* Age only; Add age of lines */
        if ((bb_rank_age > 0) && //only when age is assigned
                  (bb_age_best > norm_age_thd || bb_rank_best > norm_rank_thd
                      || AFL_R(100) < bb_select_ratio)){

          inst_ages ++;
          module_total_ages += bb_rank_age;

          bb_raw_fitness = bb_rank_age;
          bb_raw_fitness_flag = true;

          inst_fitness ++;
          module_total_fitness += bb_raw_fitness;
          
        }
      } else if (use_cmd_change && !use_cmd_age_rank && !use_cmd_age){
        /* Change Only; Add changes of lines */
        if ((bb_burst_best > 0) && //only when change is assigned
                (bb_burst_best > norm_change_thd || AFL_R(100) < bb_select_ratio)){
          inst_changes++;
          module_total_changes += bb_burst_best;

          bb_raw_fitness = bb_burst_best;
          bb_raw_fitness_flag = true;

          inst_fitness ++;
          module_total_fitness += bb_raw_fitness;
        }
      } else if ((use_cmd_age || use_cmd_age_rank) && use_cmd_change){
        /* both age and change are enabled */
        /* Note: based on normolization, 
                we skip BBs when either bb_rank_age=0 or bb_burst_best=0 */
        if ((bb_rank_age > 0 && bb_burst_best > 0) &&
                (bb_burst_best > norm_change_thd || bb_age_best > norm_age_thd
                   || bb_rank_best > norm_rank_thd || AFL_R(100) < bb_select_ratio)){
            // change
            inst_changes++;
            module_total_changes += bb_burst_best;
            
            // age
            inst_ages ++;
            module_total_ages += bb_rank_age;
            
            // combine
            bb_raw_fitness = bb_burst_best * bb_rank_age;
            bb_raw_fitness_flag = true;

            inst_fitness ++;
            module_total_fitness += bb_raw_fitness;
          
        }
        
      }

      if (bb_raw_fitness_flag) {
        Constant *Weight = ConstantFP::get(DoubleTy, bb_raw_fitness);
        Constant *MapLoc = ConstantInt::get(Int32Ty, MAP_SIZE);
        Constant *MapCntLoc = ConstantInt::get(Int32Ty, MAP_SIZE + 8);
        
        // add to shm, churn raw fitness
        Value *MapWtPtr = IRB.CreateGEP(MapPtr, MapLoc);
        LoadInst *MapWt = IRB.CreateLoad(DoubleTy, MapWtPtr);
        MapWt->setMetadata(NoSanMetaId, NoneMetaNode);
        Value *IncWt = IRB.CreateFAdd(MapWt, Weight);
        IRB.CreateStore(IncWt, MapWtPtr)
          ->setMetadata(NoSanMetaId, NoneMetaNode);

        // add to shm, block count
#ifdef WORD_SIZE_64
        Value *MapCntPtr = IRB.CreateGEP(MapPtr, MapCntLoc);
        LoadInst *MapCnt = IRB.CreateLoad(Int64Ty, MapCntPtr);
        MapCnt->setMetadata(NoSanMetaId, NoneMetaNode);
        Value *IncCnt = IRB.CreateAdd(MapCnt, ConstantInt::get(Int64Ty, 1));
        IRB.CreateStore(IncCnt, MapCntPtr)
                ->setMetadata(NoSanMetaId, NoneMetaNode);
#else
        Value *MapCntPtr = IRB.CreateGEP(MapPtr, MapCntLoc);
        LoadInst *MapCnt = IRB.CreateLoad(Int32Ty, MapCntPtr);
        MapCnt->setMetadata(NoSanMetaId, NoneMetaNode);
        Value *IncCnt = IRB.CreateAdd(MapCnt, ConstantInt::get(Int32Ty, 1));
        IRB.CreateStore(IncCnt, MapCntPtr)
                ->setMetadata(NoSanMetaId, NoneMetaNode);

#endif
      }

      inst_blocks++;

    }
  }

  /* Say something nice. */

  if (!be_quiet) {

    if (!inst_blocks) WARNF("No instrumentation targets found.");
    else OKF("Instrumented %u locations (%s mode, ratio %u%%).",
             inst_blocks, getenv("AFL_HARDEN") ? "hardened" :
             ((getenv("AFL_USE_ASAN") || getenv("AFL_USE_MSAN")) ?
              "ASAN/MSAN" : "non-hardened"), inst_ratio);
    OKF("AFLChurn instrumentation ratio: %u%% randomly + top churn", bb_select_ratio);
    if (inst_ages) module_ave_ages = module_total_ages / inst_ages;
    if (inst_changes) module_ave_chanegs = module_total_changes / inst_changes;
    if (inst_fitness) module_ave_fitness = module_total_fitness / inst_fitness;

    if (use_cmd_age && !is_one_commit){
      OKF("Using Age. Counted %u BBs with the average of f(days)=%.6f ages.", 
              inst_ages, module_ave_ages);
    } else if (use_cmd_age_rank && !is_one_commit){
      OKF("Using Rank. Counted %u BBs with the average age of f(rank)=%.6f commits.", 
                  inst_ages, module_ave_ages);
    }
    if (use_cmd_change && !is_one_commit){
      OKF("Using Change. Counted %u BBs with the average churn of f(changes)=%.6f churns.",
                    inst_changes, module_ave_chanegs);
    } 

    OKF("BB Churn Raw Fitness. Instrumented %u BBs with average raw fitness of %.6f",
                    inst_fitness, module_ave_fitness);

    OKF("Thresholds: #changes:%u, age(days):%u, ranks:%u", 
                changes_inst_thred, age_inst_thred, rank_inst_thred);

  }

  return true;

}


static void registerAFLPass(const PassManagerBuilder &,
                            legacy::PassManagerBase &PM) {

  PM.add(new AFLCoverage());

}

// static RegisterStandardPasses RegisterAFLPass(
//     PassManagerBuilder::EP_ModuleOptimizerEarly, registerAFLPass);

static RegisterStandardPasses RegisterAFLPass(
    PassManagerBuilder::EP_OptimizerLast, registerAFLPass);


static RegisterStandardPasses RegisterAFLPass0(
    PassManagerBuilder::EP_EnabledOnOptLevel0, registerAFLPass);
