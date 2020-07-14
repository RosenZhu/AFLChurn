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

      // void setValueNonSan(Value *v);
      // void setInsNonSan(Instruction *ins);
      

  };

}


char AFLCoverage::ID = 0;

// void AFLCoverage::setValueNonSan(Value *v) {
//   if (Instruction *ins = dyn_cast<Instruction>(v))
//     setInsNonSan(ins);
// }

// void AFLCoverage::setInsNonSan(Instruction *ins) {
//   if (ins)
//     ins->setMetadata(NoSanMetaId, NoneMetaNode);
// }


bool startsWith(std::string big_str, std::string small_str){
  if (big_str.compare(0, small_str.length(), small_str) == 0) return true;
  else return false;
}

/*
  Calculate score for a new source file, which is just met.
  One file is calculated only once.
  filename: relative path to file
*/
bool calScore4NewFile(git_repository *repo, const std::string &filename, std::map<std::string, std::map<unsigned int, u32>> &all_scores){
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
  u32 lsc;

  std::map<unsigned int, u32> line_score;

  if(!git_blame_file(&blame, repo, filename.c_str(), &blameopts)){

    if (git_oid_is_zero(&blameopts.newest_commit))
      strcpy(spec, "HEAD");
    else
      git_oid_tostr(spec, sizeof(spec), &blameopts.newest_commit);

    strcat(spec, ":");
    strcat(spec, filename.c_str());

    if (git_revparse_single(&obj, repo, spec)){
      if (blame) git_blame_free(blame);
      return false;
    }

    if (git_blob_lookup(&blob, repo, git_object_id(obj))){
      if (blame) git_blame_free(blame);
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
          lsc = (cur_time - commit_time) / 86400; // days; the smaller, the more important
          if (lsc == 0) line_score[line] = 0;
          else line_score[line] = (log(lsc) / log(2)) * WEIGHT_FAC; // base 2
        }
      }

      i = (int)(eol - rawdata + 1);
      line++;
    }

    all_scores[filename] = line_score;

    if (blame)  git_blame_free(blame);
    if (commit) git_commit_free(commit);
    if (blob) git_blob_free(blob);

    return true;
  } else {
    if (blame)  git_blame_free(blame);
    return false;
  }
  
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

    SAYF(cCYA "afl-llvm-pass " cBRI VERSION cRST " by <lszekeres@google.com>\n");

  } else be_quiet = 1;

  /* Decide instrumentation ratio */

  char* inst_ratio_str = getenv("AFL_INST_RATIO");
  unsigned int inst_ratio = 100;

  if (inst_ratio_str) {

    if (sscanf(inst_ratio_str, "%u", &inst_ratio) != 1 || !inst_ratio ||
        inst_ratio > 100)
      FATAL("Bad value of AFL_INST_RATIO (must be between 1 and 100)");

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
  git_libgit2_init();

  int inst_blocks = 0;

  std::set<unsigned int> bb_lines;
  unsigned int line;
  std::string git_path;
  git_repository *repo = nullptr;
  int git_no_found = 1; // 0: found; otherwise, not found

  // file name (relative path): line NO. , score
  std::map<std::string, std::map<unsigned int, u32>> map_scores;

  for (auto &F : M){
    /* Get repository path and object */
    if (git_no_found){
      SmallVector<std::pair<unsigned, MDNode *>, 4> MDs;
      std::string funcdir, funcfile;
      git_buf gitDir = {0,0,0};
      F.getAllMetadata(MDs);
      for (auto &MD : MDs) {
        if (MDNode *N = MD.second) {
          if (auto *subProgram = dyn_cast<DISubprogram>(N)) {
            funcfile = subProgram->getFilename().str();
            funcdir = subProgram->getDirectory().str();

            if (!funcfile.empty()){
              // fix path here; if "funcfile" does not start with "/", use funcdir as the prefix of funcfile
              if (!startsWith(funcfile, "/")){
                funcdir.append("/");
                funcdir.append(funcfile);
                funcfile.assign(funcdir);
              }

              git_no_found = git_repository_discover(&gitDir, funcfile.c_str(), 0, "/");
              
              if (!git_no_found){
                if (git_repository_open(&repo, gitDir.ptr)) git_no_found = 1;
              }

              if (!git_no_found){
                git_path.assign(gitDir.ptr, gitDir.size);
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

      u32 bb_score = 0, ave_score = 0, ns = 0;
      
      std::string pdir("../");
   
      bb_lines.clear();
      bb_lines.insert(0);
      
      for (auto &I: BB){
  
        line = 0;
        std::string filename;
        /* Connect targets with instructions */
        DILocation *Loc = I.getDebugLoc().get(); 
        if (Loc && !git_no_found){
          filename = Loc->getFilename().str();
          line = Loc->getLine();
          if (filename.empty()){
            DILocation *oDILoc = Loc->getInlinedAt();
            if (oDILoc){
              line = oDILoc->getLine();
              filename = oDILoc->getFilename().str();
            }
          }

          /* take care of git blame path: relative to repo dir */
          if (!filename.empty()){
            if (startsWith(filename, "/")){
              // remove current string of git_path from filename
              filename.erase(0, git_path.length());  // relative path
            } else{
              // remove "../" if exists any
              while (startsWith(filename, pdir)){              
                filename.erase(0, pdir.length());
              }
            }
            
            /* calculate score of a block */
            if (!bb_lines.count(line)){
              bb_lines.insert(line);
              
              // the code file is not processed yet
              if (!map_scores.count(filename)){
                calScore4NewFile(repo, filename, map_scores);
              }

              if (map_scores.count(filename)){
                if (map_scores[filename].count(line)){
                  bb_score += map_scores[filename][line];
                  ns++;
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

      /* Add score */
      if (ns > 0){ //only when score is assigned
        ave_score = bb_score / ns;
        //std::cout << "block id: "<< cur_loc << ", bb score: " << (float)ave_score/WEIGHT_FAC << std::endl;
#ifdef WORD_SIZE_64
        Type *LargestType = Int64Ty;
        Constant *MapWtLoc = ConstantInt::get(LargestType, MAP_SIZE);
        Constant *MapCntLoc = ConstantInt::get(LargestType, MAP_SIZE + 8);
        Constant *Weight = ConstantInt::get(LargestType, ave_score);
#else
        Type *LargestType = Int32Ty;
        Constant *MapWtLoc = ConstantInt::get(LargestType, MAP_SIZE);
        Constant *MapCntLoc = ConstantInt::get(LargestType, MAP_SIZE + 4);
        Constant *Weight = ConstantInt::get(LargestType, ave_score);
#endif
        // add to shm, weight
        Value *MapWtPtr = IRB.CreateGEP(MapPtr, MapWtLoc);
        LoadInst *MapWt = IRB.CreateLoad(LargestType, MapWtPtr);
        MapWt->setMetadata(NoSanMetaId, NoneMetaNode);
        Value *IncWt = IRB.CreateAdd(MapWt, Weight);
        
        IRB.CreateStore(IncWt, MapWtPtr)
          ->setMetadata(NoSanMetaId, NoneMetaNode);
        // add to shm, block count
        Value *MapCntPtr = IRB.CreateGEP(MapPtr, MapCntLoc);
        LoadInst *MapCnt = IRB.CreateLoad(LargestType, MapCntPtr);
        MapCnt->setMetadata(NoSanMetaId, NoneMetaNode);
        Value *IncrCnt = IRB.CreateAdd(MapCnt, ConstantInt::get(LargestType, 1));
        IRB.CreateStore(IncrCnt, MapCntPtr)
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

  }

  if (repo != nullptr)
    git_repository_free(repo);

  git_libgit2_shutdown();

  // release map
  for (auto it = map_scores.begin(); it != map_scores.end(); ++it){
    it->second.clear();
    map_scores.erase(it);
  }

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
