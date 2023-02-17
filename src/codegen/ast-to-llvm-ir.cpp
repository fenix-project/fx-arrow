
// note: arrow will not be used yet
// #include <arrow/api.h>
// #include <arrow/csv/api.h>
// #include <arrow/io/api.h>
// #include <arrow/ipc/api.h>
// #include <arrow/pretty_print.h>
// #include <arrow/result.h>
// #include <arrow/status.h>
// #include <arrow/table.h>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/Optional.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/Analysis/BasicAliasAnalysis.h>
#include <llvm/Analysis/Passes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Transforms/Scalar.h>

#include "codegen/ast-to-llvm-ir.h"
#include "codegen/ast-to-object.h"
#include "error.h"
#include "io.h"
#include "jit.h"
#include "lexer.h"
#include "parser.h"

extern std::string INPUT_FILE;
extern std::string OUTPUT_FILE;
extern std::string ARX_VERSION;

auto ASTToLLVMIRVisitor::CreateFunctionType(unsigned NumArgs)
  -> llvm::DISubroutineType* {
  llvm::SmallVector<llvm::Metadata*, 8> EltTys;
  llvm::DIType* DblTy = this->getDoubleTy();

  // Add the result type.
  EltTys.push_back(DblTy);

  for (unsigned i = 0, e = NumArgs; i != e; ++i) {
    EltTys.push_back(DblTy);
  }

  return this->DBuilder->createSubroutineType(
    this->DBuilder->getOrCreateTypeArray(EltTys));
}

// DebugInfo

auto ASTToLLVMIRVisitor::getDoubleTy() -> llvm::DIType* {
  if (this->DblTy) {
    return this->DblTy;
  }

  DblTy =
    this->DBuilder->createBasicType("double", 64, llvm::dwarf::DW_ATE_float);
  return DblTy;
}

auto ASTToLLVMIRVisitor::emitLocation(ExprAST* AST) -> void {
  if (!AST) {
    return this->Builder->SetCurrentDebugLocation(llvm::DebugLoc());
  }

  llvm::DIScope* Scope;
  if (this->LexicalBlocks.empty()) {
    Scope = TheCU;
  } else {
    Scope = this->LexicalBlocks.back();
  }

  this->Builder->SetCurrentDebugLocation(llvm::DILocation::get(
    Scope->getContext(), AST->getLine(), AST->getCol(), Scope));
}

/**
 * @brief Code generation for NumberExprAST.
 *
 */
auto ASTToLLVMIRVisitor::visit(NumberExprAST* expr) -> void {
  this->emitLocation(expr);
  ASTToObjectVisitor::visit(expr);
}

/**
 * @brief Code generation for VariableExprAST.
 *
 */
auto ASTToLLVMIRVisitor::visit(VariableExprAST* expr) -> void {
  this->emitLocation(expr);
  ASTToObjectVisitor::visit(expr);
}

/**
 * @brief Code generation for UnaryExprAST.
 *
 */
auto ASTToLLVMIRVisitor::visit(UnaryExprAST* expr) -> void {
  this->emitLocation(expr);
  ASTToObjectVisitor::visit(expr);
}

/**
 * @brief Code generation for BinaryExprAST.
 *
 */
auto ASTToLLVMIRVisitor::visit(BinaryExprAST* expr) -> void {
  this->emitLocation(expr);
  ASTToObjectVisitor::visit(expr);
}

/**
 * @brief Code generation for CallExprAST.
 *
 */
auto ASTToLLVMIRVisitor::visit(CallExprAST* expr) -> void {
  this->emitLocation(expr);
  ASTToObjectVisitor::visit(expr);
}

/**
 * @brief Code generation for IfExprAST.
 */
auto ASTToLLVMIRVisitor::visit(IfExprAST* expr) -> void {
  this->emitLocation(expr);
  ASTToObjectVisitor::visit(expr);
}

/**
 * @brief Code generation for ForExprAST.
 *
 * @param expr A `for` expression.
 */
auto ASTToLLVMIRVisitor::visit(ForExprAST* expr) -> void {
  this->emitLocation(expr);
  ASTToObjectVisitor::visit(expr);
}

/**
 * @brief Code generation for VarExprAST.
 *
 */
auto ASTToLLVMIRVisitor::visit(VarExprAST* expr) -> void {
  this->emitLocation(expr);
  ASTToObjectVisitor::visit(expr);
}

/**
 * @brief Code generation for PrototypeExprAST.
 *
 */
auto ASTToLLVMIRVisitor::visit(PrototypeAST* expr) -> void {
  ASTToObjectVisitor::visit(expr);
}

/**
 * @brief Code generation for FunctionExprAST.
 *
 * Transfer ownership of the prototype to the FunctionProtos map, but
 * keep a reference to it for use below.
 */
auto ASTToLLVMIRVisitor::visit(FunctionAST* expr) -> void {
  auto& P = *(expr->Proto);
  FunctionProtos[expr->Proto->getName()] = std::move(expr->Proto);
  this->getFunction(P.getName());
  llvm::Function* TheFunction = this->result_func;

  if (!TheFunction) {
    this->result_func = nullptr;
    return;
  }

  // Create a new basic block to start insertion into.
  // std::cout << "Create a new basic block to start insertion into";
  llvm::BasicBlock* BB =
    llvm::BasicBlock::Create(*this->TheContext, "entry", TheFunction);
  this->Builder->SetInsertPoint(BB);

  /* debugging-code:start*/
  // Create a subprogram DIE for this function.
  llvm::DIFile* Unit = this->DBuilder->createFile(
    this->TheCU->getFilename(), this->TheCU->getDirectory());
  llvm::DIScope* FContext = Unit;
  unsigned LineNo = P.getLine();
  unsigned ScopeLine = LineNo;
  llvm::DISubprogram* SP = this->DBuilder->createFunction(
    FContext,
    P.getName(),
    llvm::StringRef(),
    Unit,
    LineNo,
    CreateFunctionType(TheFunction->arg_size()),
    ScopeLine,
    llvm::DINode::FlagPrototyped,
    llvm::DISubprogram::SPFlagDefinition);
  TheFunction->setSubprogram(SP);

  // Push the current scope.
  this->LexicalBlocks.push_back(SP);

  // Unset the location for the prologue emission (leading instructions with no
  // location in a function are considered part of the prologue and the
  // debugger will run past them when breaking on a function)
  this->emitLocation(nullptr);
  /* debugging-code:end*/

  // Record the function arguments in the NamedValues map.
  // std::cout << "Record the function arguments in the NamedValues map.";
  this->NamedValues.clear();

  unsigned ArgIdx = 0;
  for (auto& Arg : TheFunction->args()) {
    // Create an alloca for this variable.
    llvm::AllocaInst* Alloca =
      CreateEntryBlockAlloca(TheFunction, Arg.getName());

    /* debugging-code: start */
    // Create a debug descriptor for the variable.
    llvm::DILocalVariable* D = this->DBuilder->createParameterVariable(
      SP, Arg.getName(), ++ArgIdx, Unit, LineNo, this->getDoubleTy(), true);

    this->DBuilder->insertDeclare(
      Alloca,
      D,
      this->DBuilder->createExpression(),
      llvm::DILocation::get(SP->getContext(), LineNo, 0, SP),
      this->Builder->GetInsertBlock());

    /* debugging-code-end */

    // Store the initial value into the alloca.
    this->Builder->CreateStore(&Arg, Alloca);

    // Add arguments to variable symbol table.
    this->NamedValues[std::string(Arg.getName())] = Alloca;
  }

  this->emitLocation(expr->Body.get());

  expr->Body->accept(this);
  llvm::Value* RetVal = this->result_val;

  if (RetVal) {
    // Finish off the function.
    this->Builder->CreateRet(RetVal);

    // Pop off the lexical block for the function.
    this->LexicalBlocks.pop_back();

    // Validate the generated code, checking for consistency.
    verifyFunction(*TheFunction);

    this->result_func = TheFunction;
    return;
  }

  // Error reading body, remove function.
  TheFunction->eraseFromParent();

  this->result_func = nullptr;

  // Pop off the lexical block for the function since we added it
  // unconditionally.
  this->LexicalBlocks.pop_back();
}

/**
 * @brief Initialize LLVM Module And PassManager.
 *
 */
auto ASTToLLVMIRVisitor::Initialize() -> void {
  ASTToObjectVisitor::Initialize();

  this->TheJIT = this->ExitOnErr(llvm::orc::ArxJIT::Create());
  this->TheModule->setDataLayout(this->TheJIT->getDataLayout());

  /** Create a new builder for the module. */
  this->DBuilder = std::make_unique<llvm::DIBuilder>(*this->TheModule);
}

/**
 * @brief Compile an AST to object file.
 *
 * @param ast The AST tree object.
 */
auto compile_llvm_ir(TreeAST* ast) -> void {
  auto codegen = new ASTToLLVMIRVisitor();

  Lexer::getNextToken();

  // Initialize the target registry etc.
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

  codegen->Initialize();

  // Run the main "interpreter loop" now.
  LOG(INFO) << "Starting MainLoop";

  // Create the compile unit for the module.
  // Currently down as "fib.ks" as a filename since we're redirecting stdin
  // but we'd like actual source locations.
  codegen->TheCU = codegen->DBuilder->createCompileUnit(
    llvm::dwarf::DW_LANG_C,
    codegen->DBuilder->createFile("fib.ks", "."),
    "Arx Compiler",
    false,
    "",
    0);

  LOG(INFO) << "Initialize Target";

  // Add the current debug info version into the module.
  codegen->TheModule->addModuleFlag(
    llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);

  // Darwin only supports dwarf2.
  if (llvm::Triple(llvm::sys::getProcessTriple()).isOSDarwin()) {
    codegen->TheModule->addModuleFlag(
      llvm::Module::Warning, "Dwarf Version", 2);
  }

  // Construct the DIBuilder, we do this here because we need the module.
  codegen->DBuilder = std::make_unique<llvm::DIBuilder>(*codegen->TheModule);

  // Create the compile unit for the module.
  // Currently down as "fib.ks" as a filename since we're redirecting stdin
  // but we'd like actual source locations.
  codegen->TheCU = codegen->DBuilder->createCompileUnit(
    llvm::dwarf::DW_LANG_C,
    codegen->DBuilder->createFile("fib.arxks", "."),
    "Arx Compiler",
    false,
    "",
    0);

  // Run the main "interpreter loop" now.
  codegen->MainLoop(ast);

  // Finalize the debug info.
  codegen->DBuilder->finalize();

  // Print out all of the generated code.
  codegen->TheModule->print(llvm::errs(), nullptr);
}

/**
 * @brief Open the Arx shell.
 *
 */
auto open_shell_llvm_ir() -> void {
  // Prime the first token.
  fprintf(stderr, "Arx %s \n", ARX_VERSION.c_str());
  fprintf(stderr, ">>> ");

  compile_llvm_ir(new TreeAST());

  exit(0);
}