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
#include <llvm/ExecutionEngine/Orc/ObjectTransformLayer.h>
#include <llvm/ExecutionEngine/Orc/DebugUtils.h>

class MyJIT {
 private:
  const bool print_generated_code = true;
  const bool dump_compiled_object_files = true;
  llvm::orc::ExecutionSession ES;
  llvm::orc::JITTargetMachineBuilder JTMB;
  llvm::DataLayout DL;
  llvm::orc::MangleAndInterner Mangle;
  llvm::orc::RTDyldObjectLinkingLayer LinkingLayer;
  llvm::orc::ObjectTransformLayer::TransformFunction DumpObjectTransform;
  llvm::orc::ObjectTransformLayer DumpObjectTransformLayer;
  llvm::orc::IRCompileLayer CompileLayer;
  llvm::orc::IRTransformLayer PrintOptimizedIRLayer;
  llvm::orc::IRTransformLayer TransformLayer;
  llvm::orc::IRTransformLayer PrintGeneratedIRLayer;
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
        DumpObjectTransform{llvm::orc::DumpObjects("generated_code/")},
        DumpObjectTransformLayer(ES, LinkingLayer,
                                 [&transform = this->DumpObjectTransform,
                                  dump_compiled_object_files = this->dump_compiled_object_files](
                                     std::unique_ptr<llvm::MemoryBuffer> buf)
                                     -> llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>> {
                                   if (dump_compiled_object_files) {
                                     return transform(std::move(buf));
                                   } else {
                                     return std::move(buf);
                                   }
                                 }),
        CompileLayer(ES, DumpObjectTransformLayer,
                     std::make_unique<llvm::orc::ConcurrentIRCompiler>(JTMB)),
        PrintOptimizedIRLayer(
            ES, CompileLayer,
            [print_generated_code = this->print_generated_code](
                llvm::orc::ThreadSafeModule TSM, const llvm::orc::MaterializationResponsibility &R)
                -> llvm::Expected<llvm::orc::ThreadSafeModule> {
              if (print_generated_code) {
                return printIR(std::move(TSM), "_opt");
              }
              return std::move(TSM);
            }),
        TransformLayer(ES, PrintOptimizedIRLayer,
                       [TM = JTMB.createTargetMachine()](
                           llvm::orc::ThreadSafeModule TSM,
                           const llvm::orc::MaterializationResponsibility &R) mutable
                       -> llvm::Expected<llvm::orc::ThreadSafeModule> {
                         if (!TM) return TM.takeError();
                         return optimizeModule(std::move(TSM), std::move(TM.get()));
                       }),
        PrintGeneratedIRLayer(
            ES, TransformLayer,
            [print_generated_code = this->print_generated_code](
                llvm::orc::ThreadSafeModule TSM, const llvm::orc::MaterializationResponsibility &R)
                -> llvm::Expected<llvm::orc::ThreadSafeModule> {
              if (print_generated_code) {
                return printIR(std::move(TSM), "");
              }
              return std::move(TSM);
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

    return PrintGeneratedIRLayer.add(MainJD, std::move(TSM));
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

  static llvm::Expected<llvm::orc::ThreadSafeModule> printIR(llvm::orc::ThreadSafeModule TSM,
                                                             const std::string &suffix = "") {
    TSM.withModuleDo([&suffix](llvm::Module &m) {
      std::error_code EC;
      const std::string output_directory = "generated_code/";
      const std::string output_file = m.getName().str() + suffix + ".ll";
      llvm::sys::fs::create_directory(output_directory, /*ignoreExisting=*/true);

      llvm::raw_fd_ostream out(output_directory + output_file, EC,
                               llvm::sys::fs::OpenFlags::OF_None);
      m.print(out, nullptr, false, true);
    });
    return TSM;
  }
};

#endif  // MyJIT_HPP