#ifndef MyJIT_HPP
#define MyJIT_HPP

#include <llvm/ADT/StringRef.h>
#include <llvm/ExecutionEngine/Orc/CompileUtils.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/ExecutorProcessControl.h>
#include <llvm/ExecutionEngine/Orc/IRCompileLayer.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/Shared/ExecutorSymbolDef.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/ExecutionEngine/JITEventListener.h>
#include <llvm/ExecutionEngine/Orc/IRTransformLayer.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/IR/Verifier.h>

class MyJIT {
 private:
  llvm::orc::ExecutionSession ES;
  llvm::orc::JITTargetMachineBuilder JTMB;
  llvm::DataLayout DL;
  llvm::orc::MangleAndInterner Mangle;
  llvm::orc::RTDyldObjectLinkingLayer LinkingLayer;
  llvm::orc::IRCompileLayer CompileLayer;
  llvm::orc::IRTransformLayer TransformLayer;
  llvm::orc::JITDylib &MainJD;

 public:
  MyJIT()
      : ES{llvm::cantFail(llvm::orc::SelfExecutorProcessControl::Create())},
        JTMB(llvm::cantFail(llvm::orc::JITTargetMachineBuilder::detectHost())
                 .setCodeGenOptLevel(llvm::CodeGenOpt::Aggressive)
                 .setCodeModel(llvm::CodeModel::Model::Large)),
        DL(llvm::cantFail(JTMB.getDefaultDataLayoutForTarget())),
        Mangle(ES, DL),
        LinkingLayer(ES, []() { return std::make_unique<llvm::SectionMemoryManager>(); }),
        CompileLayer(ES, LinkingLayer, std::make_unique<llvm::orc::ConcurrentIRCompiler>(JTMB)),
        TransformLayer(ES, CompileLayer,
                       [TM = JTMB.createTargetMachine()](
                           llvm::orc::ThreadSafeModule TSM,
                           const llvm::orc::MaterializationResponsibility &R) mutable
                       -> llvm::Expected<llvm::orc::ThreadSafeModule> {
                         if (!TM) return TM.takeError();
                         return optimizeModule(std::move(TSM), std::move(TM.get()));
                       }),
        MainJD(ES.createBareJITDylib("<main>")) {
    MainJD.addGenerator(llvm::cantFail(
        llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(DL.getGlobalPrefix())));
  }

  ~MyJIT() {
    if (auto Err = ES.endSession()) ES.reportError(std::move(Err));
  }

  const llvm::DataLayout &getDataLayout() const { return DL; }

  llvm::Error addModule(llvm::orc::ThreadSafeModule TSM) {
    bool verification_failed = TSM.withModuleDo(
        [](llvm::Module &M) -> bool { return llvm::verifyModule(M, &llvm::errs()); });
    if (verification_failed) {
      return llvm::make_error<llvm::StringError>("Module verification failed",
                                                 llvm::inconvertibleErrorCode());
    }

    return TransformLayer.add(MainJD, std::move(TSM));
  }

  llvm::Expected<llvm::orc::ExecutorSymbolDef> lookup(llvm::StringRef Name) {
    return ES.lookup({&MainJD}, Mangle(Name.str()));
  }

 private:
  static llvm::Expected<llvm::orc::ThreadSafeModule> optimizeModule(
      llvm::orc::ThreadSafeModule TSM, std::unique_ptr<llvm::TargetMachine> TM) {
    TSM.withModuleDo([&TM](llvm::Module &M) {
      llvm::LoopAnalysisManager LAM;
      llvm::FunctionAnalysisManager FAM;
      llvm::CGSCCAnalysisManager CGAM;
      llvm::ModuleAnalysisManager MAM;
      llvm::PassBuilder PB(TM.get());

      PB.registerModuleAnalyses(MAM);
      PB.registerCGSCCAnalyses(CGAM);
      PB.registerFunctionAnalyses(FAM);
      PB.registerLoopAnalyses(LAM);
      PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

      llvm::ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);

      // Optimize the IR
      MPM.run(M, MAM);
    });
    return TSM;
  }
};

#endif  // MyJIT_HPP