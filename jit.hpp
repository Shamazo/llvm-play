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
#include <llvm/ExecutionEngine/JITEventListener.h>
#include "DebugIR.hpp"

class MyJIT {
 private:
  const bool insert_preopt_debug_info = true;
  const bool insert_postopt_debug_info = false;
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
  llvm::JITEventListener *PerfListener;
  llvm::JITEventListener *GDBListener;

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
            [print_generated_code = this->print_generated_code,
             insert_debug_info = this->insert_postopt_debug_info](
                llvm::orc::ThreadSafeModule TSM, const llvm::orc::MaterializationResponsibility &R)
                -> llvm::Expected<llvm::orc::ThreadSafeModule> {
              if (print_generated_code) {
                return printIR(std::move(TSM), "_opt", insert_debug_info);
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
            [print_generated_code = this->print_generated_code,
             insert_debug_info = this->insert_preopt_debug_info](
                llvm::orc::ThreadSafeModule TSM, const llvm::orc::MaterializationResponsibility &R)
                -> llvm::Expected<llvm::orc::ThreadSafeModule> {
              TSM = nameInstructions(std::move(TSM));
              if (print_generated_code) {
                return printIR(std::move(TSM), "", insert_debug_info);
              }
              return std::move(TSM);
            }),
        MainJD(ES.createBareJITDylib("<main>")),
        PerfListener(llvm::JITEventListener::createPerfJITEventListener()),
        GDBListener(llvm::JITEventListener::createGDBRegistrationListener()) {
    if (PerfListener == nullptr) {
      std::cout << "Could not create Perf listener. Perhaps LLVM was not "
                   "compiled with perf support (LLVM_USE_PERF)."
                << std::endl;
    } else {
      LinkingLayer.registerJITEventListener(*PerfListener);
    }

    if (GDBListener == nullptr) {
      std::cout << "Could not create GDB listener." << std::endl;
    } else {
      LinkingLayer.registerJITEventListener(*GDBListener);
    }

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

  // based on InstructionNamerPass, but as a transform because we do not want to do any IR
  // optimization so we can print IR that is the same as the generated IR, just with renamed
  // instructions for readability
  static llvm::orc::ThreadSafeModule nameInstructions(llvm::orc::ThreadSafeModule TSM) {
    TSM.withModuleDo([](llvm::Module &M) {
      for (auto &F : M.functions()) {
        for (auto &Arg : F.args()) {
          if (!Arg.hasName()) Arg.setName("arg");
        }

        for (llvm::BasicBlock &BB : F) {
          if (!BB.hasName()) BB.setName("bb");

          for (llvm::Instruction &I : BB) {
            if (!I.hasName() && !I.getType()->isVoidTy()) I.setName("i");
          }
        }
      }
    });
    return TSM;
  }

  static llvm::Expected<llvm::orc::ThreadSafeModule> printIR(llvm::orc::ThreadSafeModule TSM,
                                                             const std::string &suffix = "",
                                                             bool add_debug_info = false) {
    TSM.withModuleDo([&suffix, &add_debug_info](llvm::Module &m) {
      std::error_code EC;
      const std::string output_directory = "generated_code/";
      const std::string output_file = m.getName().str() + suffix + ".ll";
      llvm::sys::fs::create_directory(output_directory, /*ignoreExisting=*/true);

      llvm::raw_fd_ostream out(output_directory + output_file, EC,
                               llvm::sys::fs::OpenFlags::OF_None);
      m.print(out, nullptr, false, true);
      if (add_debug_info) {
        llvm::createDebugInfo(m, output_directory, output_file);
      }
    });
    return TSM;
  }
};

#endif  // MyJIT_HPP