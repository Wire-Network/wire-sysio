#pragma once

#include "Inline/BasicTypes.h"
#include "IR/Module.h"

#include "llvm/IR/Module.h"

#include <vector>
#include <map>

namespace sysio { namespace chain { namespace sysvmoc {

namespace LLVMJIT {
   bool getFunctionIndexFromExternalName(const char* externalName,Uptr& outFunctionDefIndex);
   const char* getTableSymbolName();
   llvm::Module* emitModule(const IR::Module& module);
}
}}}
