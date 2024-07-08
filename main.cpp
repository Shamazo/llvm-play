#include <iostream>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/CodeGen/TargetPassConfig.h>
#include <llvm/MC/TargetRegistry.h>

#include "jit.hpp"

static std::unique_ptr<llvm::LLVMContext> TheContext;
static std::unique_ptr<llvm::Module> TheModule;
static std::unique_ptr<llvm::IRBuilder<>> TheBuilder;
static std::unique_ptr<MyJIT> TheJIT;
static llvm::ExitOnError ExitOnErr;

void initializeLLVM() {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();
  TheJIT = std::make_unique<MyJIT>();
  TheContext = std::make_unique<llvm::LLVMContext>();
  TheModule = std::make_unique<llvm::Module>("my_module", *TheContext);
  auto TargetTriple = llvm::sys::getDefaultTargetTriple();
  TheModule->setTargetTriple(TargetTriple);
  TheModule->setDataLayout(TheJIT->getDataLayout());
  TheBuilder = std::make_unique<llvm::IRBuilder<>>(*TheContext);
}

llvm::Function* createAddFunction() {
  // Create the function type
  llvm::Type* int32Type = llvm::Type::getInt32Ty(*TheContext);
  std::vector<llvm::Type*> argTypes = {int32Type, int32Type};
  llvm::FunctionType* funcType = llvm::FunctionType::get(int32Type, argTypes, false);

  // Create the function
  llvm::Function* addFunc =
      llvm::Function::Create(funcType, llvm::Function::ExternalLinkage, "add", *TheModule);
  addFunc->getArg(0)->setName("a");
  addFunc->getArg(1)->setName("b");

  // Create the entry basic block
  llvm::BasicBlock* entryBB = llvm::BasicBlock::Create(*TheContext, "entry", addFunc);
  TheBuilder->SetInsertPoint(entryBB);

  // Get the function arguments
  llvm::Argument* arg1 = &*addFunc->arg_begin();
  llvm::Argument* arg2 = &*(addFunc->arg_begin() + 1);

  // Perform the addition
  llvm::Value* result = TheBuilder->CreateAdd(arg1, arg2);

  // and return the result
  TheBuilder->CreateRet(result);
  return addFunc;
}

llvm::Function* createBuggyAddFunction() {
  // Create the function type
  llvm::Type* int32Type = llvm::Type::getInt32Ty(*TheContext);
  std::vector<llvm::Type*> argTypes = {int32Type, int32Type};
  llvm::FunctionType* funcType = llvm::FunctionType::get(int32Type, argTypes, false);

  // Create the function
  llvm::Function* buggyAddFunc =
      llvm::Function::Create(funcType, llvm::Function::ExternalLinkage, "buggyAdd", *TheModule);
  buggyAddFunc->getArg(0)->setName("a");
  buggyAddFunc->getArg(1)->setName("b");

  // Create the entry basic block
  llvm::BasicBlock* entryBB = llvm::BasicBlock::Create(*TheContext, "entry", buggyAddFunc);
  TheBuilder->SetInsertPoint(entryBB);

  // Get the function arguments
  llvm::Argument* arg1 = &*buggyAddFunc->arg_begin();
  llvm::Argument* arg2 = &*(buggyAddFunc->arg_begin() + 1);
  // Perform the addition
  llvm::Value* result = TheBuilder->CreateAdd(arg1, arg2);

  // (intentionally) segfault
  llvm::Constant* badAddress = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*TheContext), 42);
  llvm::Value* badPtr =
      llvm::ConstantExpr::getIntToPtr(badAddress, llvm::PointerType::getUnqual(int32Type));
  llvm::Value* deref = TheBuilder->CreateLoad(int32Type, badPtr);

  // Use the load to prevent it being compiled out
  result = TheBuilder->CreateAdd(result, deref);

  // and return the result
  TheBuilder->CreateRet(result);
  return buggyAddFunc;
}

llvm::Function* createArraySumFunction() {
  // Create the function type
  llvm::Type* int32Type = llvm::Type::getInt32Ty(*TheContext);
  llvm::Type* int32PtrType = llvm::PointerType::get(int32Type, 0);
  std::vector<llvm::Type*> argTypes = {int32PtrType, int32Type};
  llvm::FunctionType* funcType = llvm::FunctionType::get(int32Type, argTypes, false);

  // Create the function
  llvm::Function* sumFunc =
      llvm::Function::Create(funcType, llvm::Function::ExternalLinkage, "arraySum", *TheModule);
  sumFunc->getArg(0)->setName("arr");
  sumFunc->getArg(1)->setName("arr_len");

  // Create the entry basic block
  llvm::BasicBlock* entryBB = llvm::BasicBlock::Create(*TheContext, "entry", sumFunc);
  TheBuilder->SetInsertPoint(entryBB);
  llvm::Value* sum_ptr = TheBuilder->CreateAlloca(int32Type, nullptr, "sum");
  TheBuilder->CreateStore(llvm::ConstantInt::get(int32Type, 0), sum_ptr);
  llvm::Value* index_ptr = TheBuilder->CreateAlloca(int32Type, nullptr, "index");
  TheBuilder->CreateStore(llvm::ConstantInt::get(int32Type, 0), index_ptr);

  // Get the function arguments
  llvm::Argument* arr = &*sumFunc->arg_begin();
  llvm::Argument* size = &*(sumFunc->arg_begin() + 1);

  // Create a loop to iterate over the array
  llvm::BasicBlock* loopCondBB = llvm::BasicBlock::Create(*TheContext, "loop_cond", sumFunc);
  llvm::BasicBlock* loopBodyBB = llvm::BasicBlock::Create(*TheContext, "loop_body", sumFunc);
  llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(*TheContext, "exit", sumFunc);

  TheBuilder->CreateBr(loopCondBB);

  TheBuilder->SetInsertPoint(loopCondBB);

  llvm::Value* condition =
      TheBuilder->CreateICmpSLT(TheBuilder->CreateLoad(int32Type, index_ptr, "loaded_index"), size);
  TheBuilder->CreateCondBr(condition, loopBodyBB, exitBB);
  TheBuilder->SetInsertPoint(loopBodyBB);

  llvm::Value* currentVal = TheBuilder->CreateLoad(
      int32Type,
      TheBuilder->CreateGEP(int32Type, arr, {TheBuilder->CreateLoad(int32Type, index_ptr)}));
  llvm::Value* nextIndex = TheBuilder->CreateAdd(TheBuilder->CreateLoad(int32Type, index_ptr),
                                                 llvm::ConstantInt::get(int32Type, 1));
  llvm::Value* newSum =
      TheBuilder->CreateAdd(TheBuilder->CreateLoad(int32Type, sum_ptr), currentVal, "newSum");

  TheBuilder->CreateStore(nextIndex, index_ptr);
  TheBuilder->CreateStore(newSum, sum_ptr);

  condition = TheBuilder->CreateICmpSLT(TheBuilder->CreateLoad(int32Type, index_ptr), size);
  TheBuilder->CreateCondBr(condition, loopBodyBB, exitBB);

  TheBuilder->SetInsertPoint(exitBB);

  llvm::Value* result = TheBuilder->CreateLoad(int32Type, sum_ptr);
  TheBuilder->CreateRet(result);

  return sumFunc;
}

int main() {
  initializeLLVM();
  // inserting into a llvm::module created in `initializeLLVM`
  createAddFunction();
  createBuggyAddFunction();
  createArraySumFunction();

  // compile our code
  auto TSM = llvm::orc::ThreadSafeModule(std::move(TheModule), std::move(TheContext));
  ExitOnErr(TheJIT->addModule(std::move(TSM)));

  auto add_symbol = ExitOnErr(TheJIT->lookup("add"));
  int (*add_fp)(int, int) = add_symbol.getAddress().toPtr<int (*)(int, int)>();

  auto array_sum_symbol = ExitOnErr(TheJIT->lookup("arraySum"));
  int (*array_sum_fp)(int*, int) = array_sum_symbol.getAddress().toPtr<int (*)(int*, int)>();

  auto buggy_add_symbol = ExitOnErr(TheJIT->lookup("buggyAdd"));
  int (*buggy_add_fp)(int, int) = buggy_add_symbol.getAddress().toPtr<int (*)(int, int)>();

  constexpr int KiB = 1024;
  constexpr int arr_size = 128 * KiB;
  int* arr = new int[arr_size];
  for (int i = 0; i < arr_size; i++) {
    arr[i] = i + 1;
  }

  std::cout << "Adding 1+2 = " << add_fp(1, 2) << std::endl;
  // for profiling, run this in a loop so it uses more CPU time relative to the rest of the program
  // for (size_t i = 0; i < 10000; i++) {
  std::cout << "sum of {1, 2, ... 131072} = " << array_sum_fp(arr, arr_size) << std::endl;
  // }
  std::cout << "(with bugs) adding 1+2 = " << buggy_add_fp(1, 2) << std::endl;

  return 0;
}
