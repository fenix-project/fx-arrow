#pragma once

#include <stdio.h>
#include <memory>


namespace llvm {
/** 
 * @brief 
 * "llvm/IR/Value.h"
 * 
 */
class Value;
}

/**
 * @brief LogError* - These are little helper functions for error handling.
 * 
 */
template <typename T>
std::unique_ptr<T> LogError(const char* Str) {
  fprintf(stderr, "Error: %s\n", Str);
  return nullptr;
}

llvm::Value* LogErrorV(const char*);
