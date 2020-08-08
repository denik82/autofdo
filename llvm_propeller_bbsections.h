#ifndef LLVM_PROFILEDATA_PROPELLERPROF_H
#define LLVM_PROFILEDATA_PROPELLERPROF_H

#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

using llvm::SmallSet;
using llvm::StringMap;
using llvm::StringRef;

namespace llvm {
namespace propeller {

static const char BASIC_BLOCK_SEPARATOR[] = ".BB.";
static const char BASIC_BLOCK_UNIFIED_CHARACTERS[] = "arf";

// This data structure is shared between lld propeller components and
// create_llvm_prof. In short, create_llvm_prof parses the binary, wraps all the
// symbol information using SymbolEntry class, whereas in Propeller, PropFile
// class parses the propeller profile (which is generated by create_llvm_prof),
// and wraps the symbol information in SymbolEntry. In other words, SymbolEntry
// is the interface shared between create_llvm_prof and Propeller.
// [create_llvm_prof refer to:
// https://github.com/shenhanc78/autofdo/tree/plo-dev]
struct SymbolEntry {
  struct BBInfo {
    enum BBInfoType : unsigned char {
      BB_NONE = 0,    // For functions.
      BB_NORMAL,      // Ordinary BB
      BB_RETURN,      // Return BB
      BB_FALLTHROUGH, // Fallthrough BB
    } type;
    bool isLandingPad;
  };

  using AliasesTy = SmallVector<StringRef, 3>;

  SymbolEntry(uint64_t o, const StringRef &n, AliasesTy &&as, uint64_t address,
              uint64_t s, bool bb = false, SymbolEntry *funcptr = nullptr)
      : ordinal(o), name(n), aliases(as), addr(address), size(s), bbTag(bb),
        bbInfo({BBInfo::BB_NONE, false}), hotTag(false),
        containingFunc(funcptr) {}

  // Unique index number across all symbols that participate linking.
  uint64_t ordinal;
  // For a function symbol, it's the full name. For a bb symbol this is only the
  // bbIndex part, which is the number of "a"s before the ".bb." part. For
  // example "8", "10", etc. Refer to Propfile::createFunctionSymbol and
  // Propfile::createBasicBlockSymbol.
  StringRef name;
  // Only valid for function (bbTag == false) symbols. And aliases[0] always
  // equals to name. For example, SymbolEntry.name = "foo", SymbolEntry.aliases
  // = {"foo", "foo2", "foo3"}.
  AliasesTy aliases;
  uint64_t addr;
  uint64_t size;
  bool bbTag; // Whether this is a basic block section symbol.
  BBInfo bbInfo;

  bool hotTag; // Whether this symbol is listed in the propeller section.
  // For bbTag symbols, this is the containing fuction pointer, for a normal
  // function symbol, this points to itself. This is neverl nullptr.
  SymbolEntry *containingFunc;

  bool canFallthrough() const {
    return bbInfo.type == BBInfo::BB_FALLTHROUGH ||
           bbInfo.type == BBInfo::BB_NORMAL || bbInfo.type == BBInfo::BB_NONE;
  }

  bool isReturnBlock() const { return bbInfo.type == BBInfo::BB_RETURN; }

  bool isLandingPadBlock() const { return bbInfo.isLandingPad; }

  bool operator<(const SymbolEntry &Other) const {
    return ordinal < Other.ordinal;
  }

  bool isFunction() const { return containingFunc == this; }

  // Return true if "symName" is a BB symbol, e.g., in the form of
  // "a.BB.funcname", and set funcName to the part after ".BB.", bbIndex to
  // before ".BB.", if the pointers are nonnull.
  static bool isBBSymbol(const StringRef &symName,
                         StringRef *funcName = nullptr,
                         StringRef *bbIndex = nullptr) {
    if (symName.empty())
      return false;
    auto r = symName.split(BASIC_BLOCK_SEPARATOR);
    if (r.second.empty())
      return false;
    for (auto *i = r.first.bytes_begin(), *j = r.first.bytes_end(); i != j; ++i)
      if (strchr(BASIC_BLOCK_UNIFIED_CHARACTERS, toLower(*i)) == NULL)
        return false;
    if (funcName)
      *funcName = r.second;
    if (bbIndex)
      *bbIndex = r.first;
    return true;
  }

  static BBInfo toBBInfo(const char c) {
    bool isLandingPad = toLower(c) != c;
    switch (toLower(c)) {
    case 'a':
      return {BBInfo::BB_NORMAL, isLandingPad};
    case 'r':
      return {BBInfo::BB_RETURN, isLandingPad};
    case 'f':
      return {BBInfo::BB_FALLTHROUGH, isLandingPad};
    default:
      assert(false);
    }
    return {BBInfo::BB_NONE, false};
  }

  struct OrdinalLessComparator {
    bool operator()(SymbolEntry *s1, SymbolEntry *s2) const {
      if (s1 && s2)
        return s1->ordinal < s2->ordinal;
      return !!s1 < !!s2;
    }
  };

  static const uint64_t INVALID_ADDRESS = uint64_t(-1);
};

} // namespace propeller
} // namespace llvm

#endif
