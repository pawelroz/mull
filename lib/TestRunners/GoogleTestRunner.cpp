#include "TestRunners/GoogleTestRunner.h"

#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/OrcMCJITReplacement.h"
#include "llvm/ExecutionEngine/GenericValue.h"
#include "llvm/ExecutionEngine/RTDyldMemoryManager.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/TargetSelect.h"

#include <chrono>

using namespace Mutang;
using namespace llvm;
using namespace std::chrono;

typedef void (*mutang_destructor_t)(void *);

struct atexit_entry {
  mutang_destructor_t destructor;
  void *arg;
  void *dso_handle;
};

const static int dtors_count = 64;
static int current_dtor = 0;
static atexit_entry dtors[dtors_count];

extern "C" int mutang__cxa_atexit(mutang_destructor_t destructor, void *arg, void *__dso_handle) {
  assert(current_dtor < dtors_count);

  /// Comment the 'for' and die
  for (int i = 0; i < current_dtor; i++) {
    if (arg == dtors[i].arg) {
      printf("dtor already registered: %d: %p\n", i, arg);
      return 0;
    }
  }

  printf("record dtor: %d: %p\n", current_dtor, arg);

  dtors[current_dtor].destructor = destructor;
  dtors[current_dtor].arg = arg;
  dtors[current_dtor].dso_handle = __dso_handle;

  current_dtor++;

  return 0;
}

void runDestructors() {
  printf("dtors: %d\n", current_dtor);
  while (current_dtor > 0) {
    current_dtor--;
    printf("cleaning dtor: %d: %p\n", current_dtor, dtors[current_dtor].arg);
    dtors[current_dtor].destructor(dtors[current_dtor].arg);
  }
}

class MutangResolver : public RuntimeDyld::SymbolResolver {
public:

  RuntimeDyld::SymbolInfo findSymbol(const std::string &Name) {
    if (Name == "___cxa_atexit") {
      return findSymbol("mutang__cxa_atexit");
    }

    if (auto SymAddr = RTDyldMemoryManager::getSymbolAddressInProcess(Name))
      return RuntimeDyld::SymbolInfo(SymAddr, JITSymbolFlags::Exported);
    return RuntimeDyld::SymbolInfo(nullptr);
  }

  RuntimeDyld::SymbolInfo findSymbolInLogicalDylib(const std::string &Name) {
    return RuntimeDyld::SymbolInfo(nullptr);
  }
};

GoogleTestRunner::GoogleTestRunner() : TM(EngineBuilder().selectTarget(
                                              Triple(), "", "",
                                              SmallVector<std::string, 1>())) {
  sys::DynamicLibrary::LoadLibraryPermanently(nullptr);
  LLVMLinkInOrcMCJITReplacement();
}

std::string GoogleTestRunner::MangleName(const llvm::StringRef &Name) {
  std::string MangledName;
  {
    raw_string_ostream Stream(MangledName);
    Mangler.getNameWithPrefix(Stream, Name, TM->createDataLayout());
  }
  return MangledName;
}

void *GoogleTestRunner::TestFunctionPointer(const llvm::Function &Function) {
  orc::JITSymbol Symbol = ObjectLayer.findSymbol(MangleName(Function.getName()), false);
  void *FPointer = reinterpret_cast<void *>(static_cast<uintptr_t>(Symbol.getAddress()));
  assert(FPointer && "Can't find pointer to function");
  return FPointer;
}

void GoogleTestRunner::runStaticCtor(llvm::Function *Ctor,
                                     ObjectFiles &ObjectFiles) {
  printf("Init: %s\n", Ctor->getName().str().c_str());

  auto Handle = ObjectLayer.addObjectSet(ObjectFiles,
                                         make_unique<SectionMemoryManager>(),
                                         make_unique<MutangResolver>());
  void *FunctionPointer = TestFunctionPointer(*Ctor);

  auto func = ((int (*)())(intptr_t)FunctionPointer);
  func();

  ObjectLayer.removeObjectSet(Handle);
}

ExecutionResult GoogleTestRunner::runTest(llvm::Function *Test,
                                          ObjectFiles &ObjectFiles) {
  auto Handle = ObjectLayer.addObjectSet(ObjectFiles,
                                         make_unique<SectionMemoryManager>(),
                                         make_unique<MutangResolver>());
  void *FunctionPointer = TestFunctionPointer(*Test);

  auto start = high_resolution_clock::now();
  auto func = ((int (*)(int, const char**))(intptr_t)FunctionPointer);
  const char *argv[] = { "mutang", NULL };
  uint64_t result = func(1, argv);
  auto elapsed = high_resolution_clock::now() - start;

  runDestructors();

  ExecutionResult Result;
  Result.RunningTime = duration_cast<std::chrono::nanoseconds>(elapsed).count();

  ObjectLayer.removeObjectSet(Handle);

  if (result == 0) {
    Result.Status = ExecutionStatus::Passed;
  } else {
    Result.Status = ExecutionStatus::Failed;
  }
  
  return Result;
}
