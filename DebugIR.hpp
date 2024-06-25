//===- llvm/Transforms/Instrumentation/DebugIR.h - Interface ----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LLVM_LICENSE.TXT for details.

/**
 * This file and DebugIR.cpp are derived from this this tool to generate debug
 * info for LLVM-IR files: https://github.com/vaivaswatha/debugir which is in
 * turn based on this unmerged PR into LLVM https://reviews.llvm.org/D40778
 * which adds a pass to LLVM for this purpose.
 *
 * It seemed more straightforward to do this as a function rather than as a pass
 * since it is out of tree.
 */

#ifndef DEBUG_IR_H
#define DEBUG_IR_H
#include <llvm/IR/Module.h>

namespace llvm {

/**
 * Insert debug information into the Module in place. This strips any existing
 * debug information in the module. The debug information simply points back to
 * the generated IR, and _not_ to the cpp code that generated it. The module
 * must also be written to a file in order for the debug information to be
 * useful.
 *
 * @note the original pass implementation and the standalone tool based on that
 * pass can clone the module and also handle writing the module to a file. We
 * choose not to do that since we already have a mechanism for writing the
 * module to a file and want to use our own naming convention.
 * @param M The module to create and insert debug info into
 * @param Directory The directory containing the llvm-ir file of the module
 * @param Filename The filename of the llvm-ir file of the module within the
 * directory
 */
void createDebugInfo(llvm::Module &M, std::string Directory, std::string Filename);

}  // namespace llvm

#endif  // DEBUG_IR_H
