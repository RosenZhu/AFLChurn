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

#include "git2.h"
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
      
      // Type *VoidTy;
      // IntegerType *Int1Ty;
      // IntegerType *Int8Ty;
      // IntegerType *Int32Ty;
      // IntegerType *Int64Ty;
      // Type *Int8PtrTy;
      // Type *Int64PtrTy;
      // GlobalVariable *AFLMapPtr;
      // GlobalVariable *AFLPrevLoc;

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

struct line_chns{
    int first_diff;
    std::map<unsigned int, u32> line2changes_map;
    std::set<u32> old_changed_lines;
};

/* This is a callback for each different line in the comparison of two neighbour commits (sorted by time). */
int older_current_and_younger_neighbor_line_diff_callback(const git_diff_delta *delta,
	const git_diff_hunk *hunk,
	const git_diff_line *line,
	void *payload){
    
    struct line_chns* gp_chns = (struct line_chns *) payload;

    if ((-1 == line->old_lineno) || (-1 == line->new_lineno)) return 0;
    else {
        // line->old_lineno will be used to find the changed line number in master commit
        gp_chns->old_changed_lines.insert(line->old_lineno); 
    }
    return 0;
}

/* This is a callback for each different line in the comparison of the current oldest commit and the HEAD commit. 
This tries to get the relationship between the changed line number and the line number in the HEAD commit. */
int older_current_and_head_line_diff_callback(const git_diff_delta *delta,
	const git_diff_hunk *hunk,
	const git_diff_line *line,
	void *payload){
    
    struct line_chns* go_chns = (struct line_chns *) payload;
    if ((-1 == line->old_lineno) || (-1 == line->new_lineno)) return 0;
    else {
        // the same old_lineno changes in diff(grand, parent)
        if (go_chns->old_changed_lines.count(line->old_lineno)){ 
            // new_lineno is the changed line in master commit
            if (go_chns->line2changes_map.count(line->new_lineno)) go_chns->line2changes_map[line->new_lineno] ++;
            else go_chns->line2changes_map[line->new_lineno] = 1;
        } 
        else if (go_chns->first_diff){
          go_chns->line2changes_map[line->new_lineno] = 1;
        }
    }

    return 0;
}

/* The number of changes for lines.
 Caution: git_object_free(obj) may or may not free the "obj".
 */
bool calculate_line_change_count(git_repository *repo, const std::string filename, 
                    std::map<std::string, std::map<unsigned int, u32>> &file2line2changes_map){
  git_oid oid;
  git_revwalk *walker = nullptr;
  git_blob *younger_neighbor_blob = NULL, *older_current_blob = NULL;
  git_blob *head_blob = NULL;
  char spec[1024] = {0};
  git_object *obj = NULL;

  // get the file contents (blob) in head commit
  strcpy(spec, "HEAD");
  strcat(spec, ":");
  strcat(spec, filename.c_str());
  if(git_revparse_single(&obj, repo, spec)) return false;
  if(git_blob_lookup(&head_blob, repo, git_object_id(obj))){
    git_object_free(obj);
    return false;
  } 
  git_object_free(obj);
  
  // create a walk to traverse commits
  if(git_revwalk_new(&walker, repo)) return false;
  // the commits are sorted by time, newest to oldest
  if (git_revwalk_sorting(walker, GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME)){
    git_blob_free(head_blob);
    git_revwalk_free(walker);
    return false;
  }
  // push the head commit to the walker
  if (git_revwalk_push_head(walker)){
    git_blob_free(head_blob);
    git_revwalk_free(walker);
    return false;
  }
  
  struct line_chns line_changes;
  line_changes.first_diff = 1;
  
  // if only one commit, the "next" commit is NULL
  while (!git_revwalk_next(&oid, walker)){
    // get file blob in current commit
    memset(spec, 0, 1024);
    if (git_oid_is_zero(&oid))
        strcpy(spec, "HEAD");
    else
        git_oid_tostr(spec, sizeof(spec), &oid);
    strcat(spec, ":");
    strcat(spec, filename.c_str());

    if(git_revparse_single(&obj, repo, spec)) break;

    if(git_blob_lookup(&older_current_blob, repo, git_object_id(obj))){
      git_object_free(obj);
      break;
    }

    git_object_free(obj);
   
    // diff current commit to a younger commit
    if (line_changes.first_diff == 0) git_diff_blobs(older_current_blob, NULL, younger_neighbor_blob, 
                                                NULL, NULL, NULL, NULL, NULL, 
                                                older_current_and_younger_neighbor_line_diff_callback, &line_changes);
    
    // update the number of changes in head commit
    git_diff_blobs(older_current_blob, NULL, head_blob, 
                    NULL, NULL, NULL, NULL, NULL, 
                    older_current_and_head_line_diff_callback, &line_changes);

    if (line_changes.first_diff == 0) git_blob_free(younger_neighbor_blob);
    younger_neighbor_blob = older_current_blob;

    if (line_changes.first_diff == 1)  line_changes.first_diff = 0;
    // only needed for two neighbour commits
    line_changes.old_changed_lines.clear();
    

  }

  git_revwalk_free(walker);
  
  if (!line_changes.line2changes_map.empty())
      file2line2changes_map[filename] = line_changes.line2changes_map;

  if (line_changes.first_diff == 0) git_blob_free(younger_neighbor_blob);
  git_blob_free(head_blob);

  return true;
}

/*
  Calculate score for a new source file, which is just met.
  One file is calculated only once.
  filename: relative path to git repo
*/
bool calculate_line_age(git_repository *repo, const std::string filename, 
                        std::map<std::string, std::map<unsigned int, u32>> &file2line2age_map){
  git_blame_options blameopts = GIT_BLAME_OPTIONS_INIT;
  git_blame *blame = NULL;
  git_commit *commit = NULL;
  time_t commit_time, cur_time = std::time(0);
  unsigned int break_on_null_hunk, line;
  char spec[1024] = {0};
  git_blob *blob;
  git_object *obj;
  const char *rawdata;
  git_object_size_t i, rawsize;
  u32 days_since_last_change;

  std::map<unsigned int, u32> line_age_days;


  if(!git_blame_file(&blame, repo, filename.c_str(), &blameopts)){

    if (git_oid_is_zero(&blameopts.newest_commit))
      strcpy(spec, "HEAD");
    else
      git_oid_tostr(spec, sizeof(spec), &blameopts.newest_commit);

    strcat(spec, ":");
    strcat(spec, filename.c_str());

    if (git_revparse_single(&obj, repo, spec)){
      git_blame_free(blame);
      return false;
    }

    if (git_blob_lookup(&blob, repo, git_object_id(obj))){
      git_blame_free(blame);
      git_object_free(obj);
      return false;
    }

    git_object_free(obj);

    rawdata = (const char*)git_blob_rawcontent(blob);
    rawsize = git_blob_rawsize(blob);

    line = 1;
    i = 0;
    break_on_null_hunk = 0;

    while(i < rawsize){
      const char *eol = (const char*)memchr(rawdata + i, '\n', (size_t)(rawsize - i));
      const git_blame_hunk *hunk = git_blame_get_hunk_byline(blame, line);

      if (break_on_null_hunk && !hunk)
        break;

      if (hunk){
        break_on_null_hunk = 1;
        if (!git_commit_lookup(&commit, repo, &hunk->final_commit_id)){
          commit_time  = git_commit_time(commit);
          days_since_last_change = (cur_time - commit_time) / 86400; // days; the smaller, the more important
          /* Use log2() to reduce the effect of large days. 
            Use "[log2(days)] * WEIGHT_FAC" to keep more information of age. */
          if (days_since_last_change == 0) line_age_days[line] = 0;
          else line_age_days[line] = (log(days_since_last_change) / log(2)) * WEIGHT_FAC; // base 2
          git_commit_free(commit);
        }
        
      }

      i = (int)(eol - rawdata + 1);
      line++;
    }

    if (!line_age_days.empty())
      file2line2age_map[filename] = line_age_days;

    git_blame_free(blame);
    git_blob_free(blob);

    return true;
  }
    
  return false;
  
  
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
  NoSanMetaId = C.getMDKindID("nosanitize");
  NoneMetaNode = MDNode::get(C, None);

  /* Show a banner */

  char be_quiet = 0;

  if (isatty(2) && !getenv("AFL_QUIET")) {

    SAYF(cCYA "afl-llvm-pass " cBRI VERSION cRST " by <burstfuzz>\n");

  } else be_quiet = 1;

  /* Decide instrumentation ratio */

  char* inst_ratio_str = getenv("AFL_INST_RATIO");
  unsigned int inst_ratio = 100;

  if (inst_ratio_str) {

    if (sscanf(inst_ratio_str, "%u", &inst_ratio) != 1 || !inst_ratio ||
        inst_ratio > 100)
      FATAL("Bad value of AFL_INST_RATIO (must be between 1 and 100)");

  }

  char use_line_age = 0, use_line_change = 0;

  if (getenv("BURST_LINE_AGE")) use_line_age = 1;
  if (getenv("BURST_LINE_CHANGE")) use_line_change = 1;

  /* Get globals for the SHM region and the previous location. Note that
     __afl_prev_loc is thread-local. */

  AFLMapPtr =
      new GlobalVariable(M, PointerType::get(Int8Ty, 0), false,
                         GlobalValue::ExternalLinkage, 0, "__afl_area_ptr");

  AFLPrevLoc = new GlobalVariable(
      M, Int32Ty, false, GlobalValue::ExternalLinkage, 0, "__afl_prev_loc",
      0, GlobalVariable::GeneralDynamicTLSModel, 0, false);

  /* Instrument all the things! */
  git_libgit2_init();

  int inst_blocks = 0, inst_ages = 0, inst_changes = 0, 
      module_total_ages = 0, module_total_changes = 0,
      module_ave_ages = 0, module_ave_chanegs = 0;

  std::set<unsigned int> bb_lines;
  unsigned int line;
  std::string git_path;
  git_repository *repo = nullptr;
  int git_no_found = 1, // 0: found; otherwise, not found
      is_one_commit = 0; // don't calculate for --depth 1

  // file name (relative path): line NO. , score
  std::map<std::string, std::map<unsigned int, u32>> map_age_scores, map_bursts_scores;

  for (auto &F : M){
    /* Get repository path and object */
    if (git_no_found && !is_one_commit){
      SmallVector<std::pair<unsigned, MDNode *>, 4> MDs;
      std::string funcdir, funcfile;
      git_buf gitDir = {0,0,0};
      F.getAllMetadata(MDs);
      for (auto &MD : MDs) {
        if (MDNode *N = MD.second) {
          if (auto *subProgram = dyn_cast<DISubprogram>(N)) {
            funcfile = subProgram->getFilename().str();
            funcdir = subProgram->getDirectory().str();

            if (!funcfile.empty() && !funcdir.empty()){
              // fix path here; if "funcfile" does not start with "/", use funcdir as the prefix of funcfile
              if (!startsWith(funcfile, "/")){
                funcdir.append("/");
                funcdir.append(funcfile);
                funcfile.assign(funcdir);
              }

              git_no_found = git_repository_discover(&gitDir, funcfile.c_str(), 0, "/");
              
              if (!git_no_found){
                git_no_found = git_repository_open(&repo, gitDir.ptr);
                /* If the entire commit history contains only one commit, it may be a third-party library. 
                      Don't calculate age or change */
                if (!git_no_found){
                  git_revwalk * walker = nullptr;
                  if (git_revwalk_new(&walker, repo)){
                    git_no_found = 1;
                    continue;
                  }

                  if (git_revwalk_sorting(walker, GIT_SORT_NONE)){
                    git_revwalk_free(walker);
                    git_no_found = 1;
                    continue;
                  }

                  if (!git_revwalk_push_head(walker)){
                    git_oid oid;
                    int count_cmts = 0;
                    while(!git_revwalk_next(&oid, walker)){
                      count_cmts++;
                      break;
                    }

                    if (!count_cmts){
                      git_no_found = 1;
                      is_one_commit = 1;
                      OKF("Shallow repository clone. Ignoring file %s.", funcfile.c_str());
                    } 

                  } else{
                    // some errors when pushing head to walk
                    git_no_found = 1;
                  }

                  git_revwalk_free(walker);
                }

              }

              if (!git_no_found){
                git_path.assign(gitDir.ptr, gitDir.size);
                // std::cout << "git path: " << git_path << std::endl;
                // remove ".git/" at the end of git_path
                std::string git_end(".git/"); 
                std::size_t pos = git_path.rfind(git_end.c_str());
                if (pos != std::string::npos) git_path.erase(pos, git_end.length());
                break;
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

      u32 bb_age_total = 0, bb_age_avg = 0, bb_age_count = 0;
      u32 bb_burst_total = 0, bb_burst_avg = 0, bb_burst_count = 0;
      
      //std::string pdir("../"), curdir("./");
   
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
              if (!bb_lines.count(line)){
                bb_lines.insert(line);
                
                // calculate line age
                if (use_line_age) {
                  if (!map_age_scores.count(clean_relative_path)){
                    calculate_line_age(repo, clean_relative_path, map_age_scores);
                  }

                  if (map_age_scores.count(clean_relative_path)){
                    if (map_age_scores[clean_relative_path].count(line)){
                      bb_age_total += map_age_scores[clean_relative_path][line];
                      bb_age_count++;
                    }
                  }
                }
                // calculate line change
                if (use_line_change){
                  if (!map_bursts_scores.count(clean_relative_path)){
                    /* the number of changes for lines */
                    calculate_line_change_count(repo, clean_relative_path, map_bursts_scores);
                  }

                  if (map_bursts_scores.count(clean_relative_path)){
                    if (map_bursts_scores[clean_relative_path].count(line)){
                      bb_burst_total += map_bursts_scores[clean_relative_path][line];
                      bb_burst_count ++;
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

      /* Add age of lines */
      if (bb_age_count > 0){ //only when age is assigned
        bb_age_avg = bb_age_total / bb_age_count;
        inst_ages ++;
        module_total_ages += bb_age_avg;
        //std::cout << "block id: "<< cur_loc << ", bb age: " << (float)bb_age_avg/WEIGHT_FAC << std::endl;
#ifdef WORD_SIZE_64
        Type *AgeLargestType = Int64Ty;
        Constant *MapAgeLoc = ConstantInt::get(AgeLargestType, MAP_SIZE);
        Constant *MapAgeCntLoc = ConstantInt::get(AgeLargestType, MAP_SIZE + 8);
        Constant *AgeWeight = ConstantInt::get(AgeLargestType, bb_age_avg);
#else
        Type *AgeLargestType = Int32Ty;
        Constant *MapAgeLoc = ConstantInt::get(AgeLargestType, MAP_SIZE);
        Constant *MapAgeCntLoc = ConstantInt::get(AgeLargestType, MAP_SIZE + 4);
        Constant *AgeWeight = ConstantInt::get(AgeLargestType, bb_age_avg);
#endif
        // add to shm, age
        Value *MapAgeWtPtr = IRB.CreateGEP(MapPtr, MapAgeLoc);
        LoadInst *MapAgeWt = IRB.CreateLoad(AgeLargestType, MapAgeWtPtr);
        MapAgeWt->setMetadata(NoSanMetaId, NoneMetaNode);
        Value *IncAgeWt = IRB.CreateAdd(MapAgeWt, AgeWeight);
        
        IRB.CreateStore(IncAgeWt, MapAgeWtPtr)
          ->setMetadata(NoSanMetaId, NoneMetaNode);
        // add to shm, block count
        Value *MapAgeCntPtr = IRB.CreateGEP(MapPtr, MapAgeCntLoc);
        LoadInst *MapAgeCnt = IRB.CreateLoad(AgeLargestType, MapAgeCntPtr);
        MapAgeCnt->setMetadata(NoSanMetaId, NoneMetaNode);
        Value *IncAgeCnt = IRB.CreateAdd(MapAgeCnt, ConstantInt::get(AgeLargestType, 1));
        IRB.CreateStore(IncAgeCnt, MapAgeCntPtr)
                ->setMetadata(NoSanMetaId, NoneMetaNode);
      }

      /* Add changes of lines */
      if (bb_burst_count > 0){ //only when change is assigned
        bb_burst_avg = bb_burst_total / bb_burst_count;
        inst_changes++;
        module_total_changes += bb_burst_avg;
        // std::cout << "block id: "<< cur_loc << ", bb change: " << bb_burst_avg << std::endl;
#ifdef WORD_SIZE_64
        Type *ChangeLargestType = Int64Ty;
        Constant *MapChangeLoc = ConstantInt::get(ChangeLargestType, MAP_SIZE + 16);
        Constant *MapChangeCntLoc = ConstantInt::get(ChangeLargestType, MAP_SIZE + 24);
        Constant *ChangeWeight = ConstantInt::get(ChangeLargestType, bb_burst_avg);
#else
        Type *ChangeLargestType = Int32Ty;
        Constant *MapChangeLoc = ConstantInt::get(ChangeLargestType, MAP_SIZE + 8);
        Constant *MapChangeCntLoc = ConstantInt::get(ChangeLargestType, MAP_SIZE + 12);
        Constant *ChangeWeight = ConstantInt::get(ChangeLargestType, bb_burst_avg);
#endif
        // add to shm, changes
        Value *MapChangeWtPtr = IRB.CreateGEP(MapPtr, MapChangeLoc);
        LoadInst *MapChangeWt = IRB.CreateLoad(ChangeLargestType, MapChangeWtPtr);
        MapChangeWt->setMetadata(NoSanMetaId, NoneMetaNode);
        Value *IncChangeWt = IRB.CreateAdd(MapChangeWt, ChangeWeight);
        
        IRB.CreateStore(IncChangeWt, MapChangeWtPtr)
          ->setMetadata(NoSanMetaId, NoneMetaNode);
        // add to shm, block count
        Value *MapChangeCntPtr = IRB.CreateGEP(MapPtr, MapChangeCntLoc);
        LoadInst *MapChangeCnt = IRB.CreateLoad(ChangeLargestType, MapChangeCntPtr);
        MapChangeCnt->setMetadata(NoSanMetaId, NoneMetaNode);
        Value *IncChangeCnt = IRB.CreateAdd(MapChangeCnt, ConstantInt::get(ChangeLargestType, 1));
        IRB.CreateStore(IncChangeCnt, MapChangeCntPtr)
                ->setMetadata(NoSanMetaId, NoneMetaNode);
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

    if (inst_ages) module_ave_ages = module_total_ages / inst_ages;
    if (inst_changes) module_ave_chanegs = module_total_changes / inst_changes;
    if (use_line_age && !is_one_commit) OKF("Use line ages. Instrumented %u BBs with the average of log2(days)=%.2f", 
                  inst_ages, (float)module_ave_ages/WEIGHT_FAC);
    if (use_line_change && !is_one_commit) OKF("Use line changes. Instrumented %u BBs with the average change of %u changes.", 
                  inst_changes, module_ave_chanegs);

  }

  if (repo != nullptr)
    git_repository_free(repo);

  git_libgit2_shutdown();

  // release map
  // for (auto it = map_age_scores.begin(); it != map_age_scores.end(); ++it){
  //   it->second.clear();
  //   map_age_scores.erase(it);
  // }

  return true;

}


static void registerAFLPass(const PassManagerBuilder &,
                            legacy::PassManagerBase &PM) {

  PM.add(new AFLCoverage());

}

// TODO: which one? early or last? - rosen

// static RegisterStandardPasses RegisterAFLPass(
//     PassManagerBuilder::EP_ModuleOptimizerEarly, registerAFLPass);

static RegisterStandardPasses RegisterAFLPass(
    PassManagerBuilder::EP_OptimizerLast, registerAFLPass);


static RegisterStandardPasses RegisterAFLPass0(
    PassManagerBuilder::EP_EnabledOnOptLevel0, registerAFLPass);
