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

using namespace llvm;

cl::opt<std::string> TargetsFile(
    "targets",
    cl::desc("Input file containing the target lines of code."),
    cl::value_desc("targets"));

namespace {

  class AFLCoverage : public ModulePass {

    public:

      static char ID;
      
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

      AFLCoverage() : ModulePass(ID) { }

      bool runOnModule(Module &M) override;

      // StringRef getPassName() const override {
      //  return "American Fuzzy Lop Instrumentation";
      // }
      void getCmpOpValue(Instruction& Inst, unsigned int cur_id);
      void getSwValue(Instruction& Inst, unsigned int cur_id);
      void setValueNonSan(Value *v);
      void setInsNonSan(Instruction *ins);

  };

}


char AFLCoverage::ID = 0;

void AFLCoverage::setValueNonSan(Value *v) {
  if (Instruction *ins = dyn_cast<Instruction>(v))
    setInsNonSan(ins);
}

void AFLCoverage::setInsNonSan(Instruction *ins) {
  if (ins)
    ins->setMetadata(NoSanMetaId, NoneMetaNode);
}

// get value of the operand in cmp instruction
void AFLCoverage::getCmpOpValue(Instruction& Instr, unsigned int cur_id){
    Instruction *InsertPoint = Instr.getNextNode();
    if (!InsertPoint || isa<ConstantInt>(Instr)) return;
   
    CmpInst *Cmp = dyn_cast<CmpInst>(&Instr);
    Value *OpArg[2];
    OpArg[0] = Cmp->getOperand(0);
    OpArg[1] = Cmp->getOperand(1);


    Type *OpType = OpArg[0]->getType();
    Value *Shv0, *Shv1;

    IRBuilder<> IRCMP(InsertPoint);

    if (OpType->isFloatTy()){
        Shv0 = IRCMP.CreateBitCast(OpArg[0], Int32Ty);
        setValueNonSan(Shv0);
        Shv0 = IRCMP.CreateZExt(Shv0, Int64Ty);
        Shv1 = IRCMP.CreateBitCast(OpArg[1], Int32Ty);
        setValueNonSan(Shv1);
        Shv1 = IRCMP.CreateZExt(Shv1, Int64Ty);
    }else if (OpType->isDoubleTy()){
        Shv0 = IRCMP.CreateBitCast(OpArg[0], Int64Ty);
        setValueNonSan(Shv0);
        Shv1 = IRCMP.CreateBitCast(OpArg[1], Int64Ty);
        setValueNonSan(Shv1);
    }else if (OpType->isPointerTy()) {
        Shv0 = IRCMP.CreatePtrToInt(OpArg[0], Int64Ty);
        Shv1 = IRCMP.CreatePtrToInt(OpArg[1], Int64Ty);
    } else if (OpType->isIntegerTy() && OpType->getIntegerBitWidth() < 64) {
        Shv0 = IRCMP.CreateZExt(OpArg[0], Int64Ty); //zero extension instruction 
        Shv1 = IRCMP.CreateZExt(OpArg[1], Int64Ty); //zero extension instruction 
    }else if (OpType->isIntegerTy() && OpType->getIntegerBitWidth() == 64){
        Shv0 = OpArg[0];
        Shv1 = OpArg[1];
    }else{ //TODO: OpType->isVectorTy()?
        return;
    }
    
    LoadInst *MapBasePtr = IRCMP.CreateLoad(AFLMapPtr); //load base SHM pointer
    MapBasePtr->setMetadata(NoSanMetaId, NoneMetaNode);

    Constant *ValID = ConstantInt::get(Int32Ty, MAP_SIZE + BB_SCORE_SIZE + cur_id * 8);
    Value *MapPtrValIndex = IRCMP.CreateGEP(MapBasePtr, ValID); //index

    /* Use XOR as the hash function of operands; 
        Check if one of the operands in a block changes when one byte in a seed is mutated: 
          if result of XOR changes, one or more of the operands change.
      TODO: change hash function? -rosen */

    Value *OPRes = IRCMP.CreateXor(Shv0, Shv1);
    LoadInst *MapOpVal = IRCMP.CreateLoad(Int64Ty, MapPtrValIndex);
    Value *memRes = IRCMP.CreateXor(OPRes, MapOpVal);

    IRCMP.CreateStore(memRes, MapPtrValIndex)
              ->setMetadata(NoSanMetaId, NoneMetaNode);

}

// get value of switch
void AFLCoverage::getSwValue(Instruction& Instr, unsigned int cur_id){
    SwitchInst *Sw = dyn_cast<SwitchInst>(&Instr);
    Value *Cond = Sw->getCondition();

    if (!(Cond && Cond->getType()->isIntegerTy() && !isa<ConstantInt>(Cond))) {
        return;
    }

    IRBuilder<> IRSW(Sw);
    Value *CondExt = IRSW.CreateZExt(Cond, Int64Ty);
  
    LoadInst *MapBasePtr = IRSW.CreateLoad(AFLMapPtr); //load base SHM pointer
    MapBasePtr->setMetadata(NoSanMetaId, NoneMetaNode);

    Constant *ValID = ConstantInt::get(Int32Ty, MAP_SIZE + BB_SCORE_SIZE + cur_id * 8);
    Value *MapPtrValIndex = IRSW.CreateGEP(MapBasePtr, ValID); //index

    /* Use XOR as the hash function of operands; 
        Check if one of the operands in a block changes when one byte in a seed is mutated: 
          if result of XOR changes, one or more of the operands change.
      TODO: change hash function? -rosen */

    LoadInst *MapOpVal = IRSW.CreateLoad(Int64Ty, MapPtrValIndex);
    Value *memRes = IRSW.CreateXor(CondExt, MapOpVal);

    IRSW.CreateStore(memRes, MapPtrValIndex)
              ->setMetadata(NoSanMetaId, NoneMetaNode);

}


bool AFLCoverage::runOnModule(Module &M) {

  /* get change-burst targets */
  std::list<std::string> targets;

  if (!TargetsFile.empty()) {
    std::ifstream targetsfile(TargetsFile);
    std::string line;
    while (std::getline(targetsfile, line))
      targets.push_back(line);
    targetsfile.close();
  }

  LLVMContext &C = M.getContext();

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

  int inst_blocks = 0;
  int bb_score;

  for (auto &F : M){

    for (auto &BB : F) {
      std::string filename;
      unsigned line;
      bb_score = 0; //score, according to change burst

      BasicBlock::iterator IP = BB.getFirstInsertionPt();
      IRBuilder<> IRB(&(*IP));

      if (AFL_R(100) >= inst_ratio) continue;

      /* Make up cur_loc */

      unsigned int cur_loc = AFL_R(MAP_SIZE);

      ConstantInt *CurLoc = ConstantInt::get(Int32Ty, cur_loc);

      for (auto &I: BB){

        /* Connect targets with instructions */
        DILocation *Loc = I.getDebugLoc().get(); 
        if (Loc){
          filename = Loc->getFilename().str();
          line = Loc->getLine();
          if (filename.empty()){
            DILocation *oDILoc = Loc->getInlinedAt();
            if (oDILoc){
              line = oDILoc->getLine();
              filename = oDILoc->getFilename().str();
            }
          }
        }
        /* score for each block */
        for (std::list<std::string>::iterator it = targets.begin(); it != targets.end(); ++it) {

          std::string target = *it;
          std::size_t found = target.find_last_of("/\\");
          if (found != std::string::npos)
            target = target.substr(found + 1);

          std::size_t pos = target.find_last_of(":");
          std::string target_file = target.substr(0, pos);
          unsigned int target_line = atoi(target.substr(pos + 1).c_str());
          
          // one target line, score +1
          if (!target_file.compare(filename) && target_line == line) bb_score++;

        }
        // TODO: necessary? - rosen
        if (bb_score > 255) bb_score = 255;

        /* XOR values of operands in a block; 
            Connect bytes in an input with operands of cmp or switch in a block; 
            More interested in bytes that can flip conditions.*/
        if (isa<CmpInst>(I)){
            getCmpOpValue(I, cur_loc);
        } else if (isa<SwitchInst>(I)){
            getSwValue(I, cur_loc);
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

      /* Assign score to the current block */
      Constant *BBScore = ConstantInt::get(Int8Ty, bb_score);
      Constant *ScoreID = ConstantInt::get(Int32Ty, MAP_SIZE  + cur_loc);
      Value *MapPtrScore = IRB.CreateGEP(MapPtr, ScoreID); //index
      IRB.CreateStore(BBScore, MapPtrScore)
              ->setMetadata(NoSanMetaId, NoneMetaNode);; //assign score

      /* Set prev_loc to cur_loc >> 1 */

      StoreInst *Store =
          IRB.CreateStore(ConstantInt::get(Int32Ty, cur_loc >> 1), AFLPrevLoc);
      Store->setMetadata(NoSanMetaId, NoneMetaNode);

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

  return true;

}


static void registerAFLPass(const PassManagerBuilder &,
                            legacy::PassManagerBase &PM) {

  PM.add(new AFLCoverage());

}


static RegisterStandardPasses RegisterAFLPass(
    PassManagerBuilder::EP_ModuleOptimizerEarly, registerAFLPass);

static RegisterStandardPasses RegisterAFLPass0(
    PassManagerBuilder::EP_EnabledOnOptLevel0, registerAFLPass);
