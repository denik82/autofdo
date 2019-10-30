#include "config.h"
#if defined(HAVE_LLVM)
#include "llvm_propeller_profile_writer.h"

#include <fstream>
#include <functional>
#include <iomanip>
#include <ios>
#include <list>
#include <numeric>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "gflags/gflags.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#include "third_party/perf_data_converter/src/quipper/perf_parser.h"
#include "third_party/perf_data_converter/src/quipper/perf_reader.h"

DEFINE_string(match_mmap_file, "", "Match mmap event file path.");
DEFINE_bool(ignore_build_id, false, "Ignore build id match.");

using llvm::dyn_cast;
using llvm::StringRef;
using llvm::object::ELFFile;
using llvm::object::ELFObjectFile;
using llvm::object::ELFObjectFileBase;
using llvm::object::ObjectFile;
using std::list;
using std::make_pair;
using std::ofstream;
using std::pair;
using std::string;
using std::stringstream;
using std::tuple;

PropellerProfWriter::PropellerProfWriter(const string &BFN, const string &PFN,
                                         const string &OFN)
    : BinaryFileName(BFN), PerfFileName(PFN), PropOutFileName(OFN) {}

PropellerProfWriter::~PropellerProfWriter() {}

SymbolEntry *PropellerProfWriter::findSymbolAtAddress(uint64_t Pid,
                                                      uint64_t OriginAddr) {
  uint64_t Addr = adjustAddressForPIE(Pid, OriginAddr);
  if (Addr == INVALID_ADDRESS) return nullptr;
  auto U = AddrMap.upper_bound(Addr);
  if (U == AddrMap.begin()) return nullptr;
  auto R = std::prev(U);

  // 99+% of the cases:
  if (R->second.size() == 1 && R->second.front()->containsAddress(Addr))
    return *(R->second.begin());

  list<SymbolEntry *> Candidates;
  for (auto &SymEnt : R->second)
    if (SymEnt->containsAddress(Addr)) Candidates.emplace_back(SymEnt);

  if (Candidates.empty()) return nullptr;

  // Sort candidates by symbol size.
  Candidates.sort([](const SymbolEntry *S1, const SymbolEntry *S2) {
    if (S1->Size != S2->Size) return S1->Size < S2->Size;
    return S1->Name < S2->Name;
  });

  // Return the smallest symbol that contains address.
  return *Candidates.begin();
}

namespace {
struct DecOut {
} dec;

struct HexOut {
} hex;

struct Hex0xOut {
} hex0x;

struct SymBaseF {
  SymBaseF(const SymbolEntry &S) : Symbol(S) {}
  const SymbolEntry &Symbol;

  static SymbolEntry dummySymbolEntry;
};

SymbolEntry SymBaseF::dummySymbolEntry(0, "", SymbolEntry::AliasesTy(),
                                       0, 0,  0);

struct SymNameF : public SymBaseF {
  SymNameF(const SymbolEntry &S) : SymBaseF(S) {}
  SymNameF(const SymbolEntry *S) : SymBaseF(*S) {}
  SymNameF(const StringRef &N)
      : SymBaseF(SymBaseF::dummySymbolEntry), Name(N) {}

  StringRef Name;
};

struct SymOrdinalF : public SymBaseF {
  SymOrdinalF(const SymbolEntry &S) : SymBaseF(S) {}
  SymOrdinalF(const SymbolEntry *S) : SymBaseF(*S) {}
};

struct SymSizeF : public SymBaseF {
  SymSizeF(const SymbolEntry &S) : SymBaseF(S) {}
};

struct SymShortF : public SymBaseF {
  SymShortF(const SymbolEntry &S) : SymBaseF(S) {}
  SymShortF(const SymbolEntry *S) : SymBaseF(*S) {}
};

struct CountF {
  CountF(uint64_t C) : Cnt(C){};
  uint64_t Cnt;
};

struct CommaF {
  CommaF(uint64_t v) : value(v) {}
  uint64_t value;
};

struct PercentageF {
  PercentageF(double d) : value(d) {}
  PercentageF(uint64_t a, uint64_t b) : value((double)a / (double)b) {}
  template<class C>
  PercentageF(C &c, uint64_t b) : value((double)c.size() / (double)b) {}
  template<class C, class D>
  PercentageF(C &c, D &d) : value((double)c.size() / (double)d.size()) {}
  double value;
};

struct BuildIdWrapper {
  BuildIdWrapper(const quipper::PerfDataProto_PerfBuildID &BuildId)
      : Data(BuildId.build_id_hash().c_str()) {}

  BuildIdWrapper(const char *P) : Data(P) {}

  const char *Data;
};

static std::ostream &operator<<(std::ostream &out, const struct DecOut &) {
  return out << std::dec << std::noshowbase;
}

static std::ostream &operator<<(std::ostream &out, const struct HexOut &) {
  return out << std::hex << std::noshowbase;
}

static std::ostream &operator<<(std::ostream &out, const struct Hex0xOut &) {
  return out << std::hex << std::showbase;
}

static std::ostream &operator<<(std::ostream &out, const SymNameF &NameF) {
  auto &Sym = NameF.Symbol;
  auto SimplifiedName = [&Sym](StringRef nameRef) -> std::string {
    auto nameSplit = nameRef.split(lld::propeller::BASIC_BLOCK_SEPARATOR);
    if (!nameSplit.second.empty())
      return std::to_string(nameSplit.first.size()) +
             nameRef.substr(nameSplit.first.size()).str();
    return nameRef.str();
  };
  if (NameF.Name.empty()) {
    out << SimplifiedName(Sym.Name);
    for (auto A : Sym.Aliases) out << "/" << SimplifiedName(A);
  } else
    out << SimplifiedName(NameF.Name);
  return out;
}

static std::ostream &operator<<(std::ostream &out,
                                const SymOrdinalF &OrdinalF) {
  return out << dec << OrdinalF.Symbol.Ordinal;
}

static std::ostream &operator<<(std::ostream &out, const SymSizeF &SizeF) {
  return out << hex << SizeF.Symbol.Size;
}

static std::ostream &operator<<(std::ostream &out, const SymShortF &SymSF) {
  return out << "symbol '" << SymNameF(SymSF.Symbol) << "@" << hex0x
             << SymSF.Symbol.Addr << "'";
}

static std::ostream &operator<<(std::ostream &out, const CountF &CountF) {
  return out << dec << CountF.Cnt;
}

static std::ostream &operator<<(std::ostream &OS, const MMapEntry &ME) {
  return OS << "[" << hex0x << ME.LoadAddr << ", " << hex0x << ME.getEndAddr()
            << "] (PgOff=" << hex0x << ME.PageOffset << ", Size=" << hex0x
            << ME.LoadSize << ")";
};

static std::ostream &operator<<(std::ostream &out, const BuildIdWrapper &BW) {
  for (int i = 0; i < quipper::kBuildIDArraySize; ++i) {
    out << std::setw(2) << std::setfill('0') << std::hex
        << ((int)(BW.Data[i]) & 0xFF);
  }
  return out;
}

// Output integer numbers in "," separated format.
static std::ostream &operator<<(std::ostream &out, const CommaF &CF) {
  std::list<int> seg;
  uint64_t value = CF.value;
  while (value) {
    seg.insert(seg.begin(), value % 1000);
    value /= 1000;
  }
  if (seg.empty()) seg.insert(seg.begin(), 0);
  auto OF = out.fill();
  auto OW = out.width();
  auto i = seg.begin();
  out << std::setfill('\0') << *i;
  for (++i; i != seg.end(); ++i)
    out << "," << std::setw(3) << std::setfill('0') << *i;
  out.fill(OF);
  out.width(OW);
  return out;
}

static std::ostream &operator<<(std::ostream &out, const PercentageF &PF) {
  out << std::setprecision(3);
  out << (PF.value * 100) << '%';
  return out;
}

}  // namespace

bool PropellerProfWriter::write() {
  if (!initBinaryFile() || !findBinaryBuildId() || !populateSymbolMap() ||
      !parsePerfData()) {
    return false;
  }

  ofstream fout(PropOutFileName);
  if (fout.bad()) {
    LOG(ERROR) << "Failed to open '" << PropOutFileName << "' for writing.";
    return false;
  }

  writeOuts(fout);
  writeSymbols(fout);
  writeBranches(fout);
  writeFallthroughs(fout);
  writeHotFuncAndBBList(fout);

  summarize();
  return true;
}

void PropellerProfWriter::summarize() {
  LOG(INFO) << "Wrote propeller profile (" << PerfDataFileParsed << " file(s), "
            << CommaF(SymbolsWritten) << " syms, " << CommaF(BranchesWritten)
            << " branches, " << CommaF(FallthroughsWritten)
            << " fallthroughs) to " << PropOutFileName;

  LOG(INFO) << CommaF(CountersNotAddressed) << " of " << CommaF(TotalCounters)
            << " branch entries are not mapped (" << std::setprecision(3)
            << PercentageF(CountersNotAddressed, TotalCounters) << ").";

  LOG(INFO) << CommaF(CrossFunctionCounters) << " of " << CommaF(TotalCounters)
            << " branch entries are cross function. (" << std::setprecision(3)
            << PercentageF(CrossFunctionCounters, TotalCounters) << ").";

  uint64_t TotalBBsWithinFuncsWithProf = 0;
  uint64_t NumBBsWithProf = 0;
  set<uint64_t> FuncsWithProf;
  for (auto &SE : HotSymbols) {
    if (FuncsWithProf.insert(SE->ContainingFunc->Ordinal).second)
      TotalBBsWithinFuncsWithProf += FuncBBCounter[SE->ContainingFunc->Ordinal];
    if (SE->BBTag)
      ++NumBBsWithProf;
  }
  uint64_t TotalFuncs = 0;
  uint64_t TotalBBsAll = 0;
  for (auto &P : this->SymbolNameMap) {
    SymbolEntry *S = P.second.get();
    if (S->BBTag)
      ++TotalBBsAll;
    else
      ++TotalFuncs;
  }
  LOG(INFO) << CommaF(TotalFuncs) << " functions, "
            << CommaF(FuncsWithProf.size()) << " functions with prof ("
            << PercentageF(FuncsWithProf, TotalFuncs) << ")"
            << ", " << CommaF(TotalBBsAll) << " BBs (average "
            << TotalBBsAll / TotalFuncs << " BBs per func), "
            << CommaF(TotalBBsWithinFuncsWithProf) << " BBs within hot funcs ("
            << PercentageF(TotalBBsWithinFuncsWithProf, TotalBBsAll) << "), "
            << CommaF(NumBBsWithProf) << " BBs with prof (include "
            << CommaF(ExtraBBsIncludedInFallthroughs)
            << " BBs that are on the path of "
               "fallthroughs, total accounted for "
            << PercentageF(NumBBsWithProf, TotalBBsAll) << " of all BBs).";
}

void PropellerProfWriter::writeOuts(ofstream &fout) {
  set<string> paths{FLAGS_match_mmap_file, BinaryMMapName, BinaryFileName};
  set<string> nameMatches;
  for (const auto &v : paths)
    if (!v.empty()) nameMatches.insert(llvm::sys::path::filename(v).str());

  for (const auto &v : nameMatches)
    if (!v.empty()) fout << "@" << v << std::endl;
}

void PropellerProfWriter::writeHotFuncAndBBList(ofstream &fout) {
  SymbolEntry *LastFuncSymbol = nullptr;
  for (auto *SE : HotSymbols)
    if (SE->BBTag) {
      if (LastFuncSymbol != SE->ContainingFunc) {
        fout << "!" << SymNameF(SE->ContainingFunc) << std::endl;
        LastFuncSymbol = SE->ContainingFunc;
      }
      fout << "!!" << SE->Name.size() << std::endl;
    } else {
      fout << "!" << SymNameF(SE) << std::endl;
      LastFuncSymbol = SE;
    }
}

void PropellerProfWriter::writeSymbols(ofstream &fout) {
  this->SymbolsWritten = 0;
  uint64_t SymbolOrdinal = 0;
  fout << "Symbols" << std::endl;
  for (auto &LE : AddrMap) {
    // Tricky case here:
    // In the same address we have:
    //    foo.bb.1
    //    foo
    // So we first output foo.bb.1, and at this time
    //   foo.bb.1->containingFunc->Index == 0.
    // We output 0.1, wrong!.
    // To handle this, we sort LE by BBTag:
    if (LE.second.size() > 1) {
      LE.second.sort([](SymbolEntry *S1, SymbolEntry *S2) {
        if (S1->BBTag != S2->BBTag) {
          return !S1->BBTag;
        }
        // order irrelevant, but choose a stable relation.
        return S1->Name < S2->Name;
      });
    }
    // Then apply ordial to all before accessing.
    for (auto *SEPtr : LE.second) {
      SEPtr->Ordinal = ++SymbolOrdinal;
    }
    for (auto *SEPtr : LE.second) {
      SymbolEntry &SE = *SEPtr;
      fout << SymOrdinalF(SE) << " " << SymSizeF(SE) << " ";
      ++this->SymbolsWritten;
      if (SE.BBTag) {
        fout << SymOrdinalF(SE.ContainingFunc) << ".";
        StringRef BBIndex = SE.Name;
        fout << dec << (uint64_t)(BBIndex.bytes_end() - BBIndex.bytes_begin())
             << std::endl;
        ++FuncBBCounter[SE.ContainingFunc->Ordinal];
      } else {
        fout << "N" << SymNameF(SE) << std::endl;
      }
    }
  }
}

void PropellerProfWriter::writeBranches(std::ofstream &fout) {
  this->BranchesWritten = 0;
  fout << "Branches" << std::endl;
  auto recordHotSymbol = [this](SymbolEntry *S) {
    if (!S || !(S->ContainingFunc) || S->ContainingFunc->Name.empty()) return;
    // Dups are properly handled by set.
    HotSymbols.insert(S);
  };

  using BrCntSummationKey = tuple<SymbolEntry *, SymbolEntry *, char>;
  struct BrCntSummationKeyComp {
    bool operator()(const BrCntSummationKey &K1,
                    const BrCntSummationKey &K2) const {
      SymbolEntry *From1, *From2, *To1, *To2;
      char T1, T2;
      std::tie(From1, To1, T1) = K1;
      std::tie(From2, To2, T2) = K2;
      if (From1->Ordinal != From2->Ordinal)
        return From1->Ordinal < From2->Ordinal;
      if (To1->Ordinal != To2->Ordinal) return To1->Ordinal < To2->Ordinal;
      return T1 < T2;
    }
  };
  using BrCntSummationTy =
      map<BrCntSummationKey, uint64_t, BrCntSummationKeyComp>;
  BrCntSummationTy BrCntSummation;

  TotalCounters = 0;
  CountersNotAddressed = 0;
  CrossFunctionCounters = 0;
  // {pid: {<from, to>: count}}
  for (auto &BCPid : BranchCountersByPid) {
    const uint64_t Pid = BCPid.first;
    auto &BC = BCPid.second;
    for (auto &EC : BC) {
      const uint64_t From = EC.first.first;
      const uint64_t To = EC.first.second;
      const uint64_t Cnt = EC.second;
      auto *FromSym = findSymbolAtAddress(Pid, From);
      auto *ToSym = findSymbolAtAddress(Pid, To);
      const uint64_t AdjustedTo = adjustAddressForPIE(Pid, To);

      recordHotSymbol(FromSym);
      recordHotSymbol(ToSym);

      TotalCounters += Cnt;
      if (!FromSym || !ToSym) CountersNotAddressed += Cnt;
      if (FromSym && ToSym) {
        if (FromSym->ContainingFunc != ToSym->ContainingFunc)
          CrossFunctionCounters += Cnt;
        /* If a return jumps to an address associated with a BB symbol ToSym,
         * then find the actual callsite symbol which is the symbol right
         * before ToSym. */
        if (ToSym->BBTag &&
            (FromSym->ContainingFunc->Addr != ToSym->ContainingFunc->Addr) &&
            ToSym->ContainingFunc->Addr != AdjustedTo &&
            AdjustedTo == ToSym->Addr) { /* implies an inter-procedural return
                                            to the end of a basic block */
          auto *CallSiteSym = findSymbolAtAddress(Pid, To - 1);
          // LOG(INFO) << std::hex << "Return From: 0x" << From << " To: 0x" <<
          // To
          //           << " Callsite symbol: 0x"
          //           << (CallSiteSym ? CallSiteSym->Addr : 0x0) << "\n"
          //           << std::dec;
          if (CallSiteSym && CallSiteSym->BBTag) {
            /* Account for the fall-through between CallSiteSym and ToSym. */
            FallthroughCountersBySymbol[make_pair(CallSiteSym, ToSym)] += Cnt;
            /* Reassign ToSym to be the actuall callsite symbol entry. */
            ToSym = CallSiteSym;
          }
        }

        char Type = ' ';
        if ((ToSym->BBTag && ToSym->ContainingFunc->Addr == AdjustedTo) ||
            (!ToSym->BBTag && ToSym->isFunction() &&
             ToSym->Addr == AdjustedTo)) {
          Type = 'C';
        } else if (AdjustedTo > ToSym->Addr) {
          // Transfer to the middle of a basic block, usually a return, either a
          // normal one or a return from recursive call, but could it be a
          // dynamic jump?
          Type = 'R';
          // fprintf(stderr, "R: From=0x%lx(0x%lx), To=0x%lx(0x%lx), Pid=%ld\n",
          //         From, adjustAddressForPIE(Pid, From), To,
          //         adjustAddressForPIE(Pid, To), Pid);
        }
        BrCntSummation[std::make_tuple(FromSym, ToSym, Type)] += Cnt;
      }
    }
  }

  for (auto &BrEnt : BrCntSummation) {
    SymbolEntry *FromSym, *ToSym;
    char Type;
    std::tie(FromSym, ToSym, Type) = BrEnt.first;
    uint64_t Cnt = BrEnt.second;
    fout << SymOrdinalF(FromSym) << " " << SymOrdinalF(ToSym) << " "
         << CountF(Cnt);
    if (Type != ' ') {
      fout << ' ' << Type;
    }
    fout << std::endl;
    ++this->BranchesWritten;
  }
}

// Compute fallthrough BBs from "From" -> "To", and place them in "Path".
// ("From" and "To" are excluded)
bool PropellerProfWriter::calculateFallthroughBBs(
    SymbolEntry *From, SymbolEntry *To, std::vector<SymbolEntry *> &Path) {
  if (From == To) return true;
  if (From->Addr > To->Addr) {
    LOG(FATAL) << "*** Internal error: fallthrough path start address is "
                  "larger than end address. ***";
    return false;
  }
  auto P = AddrMap.find(From->Addr), Q = AddrMap.find(To->Addr),
       E = AddrMap.end();
  if (P == E || Q == E) {
    LOG(FATAL) << "*** Internal error: invalid symbol in fallthrough pair. ***";
    return false;
  }
  if (From->ContainingFunc != To->ContainingFunc) {
    LOG(ERROR) << "fallthrough (" << SymShortF(From) << " -> "
               << SymShortF(To)
               << ") does not start and end within the same faunction.";
    return false;
  }
  auto Func = From->ContainingFunc;
  auto I = P;
  for (++I; I != Q && I != E; ++I) {
    SymbolEntry *LastFoundSymbol = nullptr;
    for (auto *SE : I->second) {
      if (SE->BBTag && SE->ContainingFunc == Func) {
        if (LastFoundSymbol) {
          LOG(ERROR) << "fallthrough (" << SymShortF(From) << " -> "
                     << SymShortF(To) << ") contains ambiguous "
                     << SymShortF(SE) << " and " << SymShortF(LastFoundSymbol)
                     << ".";
        }
        // Mark both ambiguous bbs as touched.
        Path.emplace_back(SE);
        LastFoundSymbol = SE;
      }
    }
    if (!LastFoundSymbol) {
      LOG(ERROR) << "failed to find a BB for "
                 << "fallthrough (" << SymShortF(*From) << " -> "
                 << SymShortF(To) << "), the last found BB is "
                 << SymShortF(*Path.rbegin());
      return false;
    }
    if (Path.size() >= 200) {
      LOG(ERROR) << "too many BBs along fallthrough (" << SymShortF(From)
                 << " -> " << SymShortF(To) << "), probably a bug.";
      return false;
    }
  }
  return true;
}

void PropellerProfWriter::writeFallthroughs(std::ofstream &fout) {
  // CAPid: {pid, <<from_addr, to_addr>, counter>}
  for (auto &CAPid : FallthroughCountersByPid) {
    uint64_t Pid = CAPid.first;
    for (auto &CA : CAPid.second) {
      const uint64_t Cnt = CA.second;
      auto *FromSym = findSymbolAtAddress(Pid, CA.first.first);
      auto *ToSym = findSymbolAtAddress(Pid, CA.first.second);
      if (FromSym && ToSym)
        FallthroughCountersBySymbol[std::make_pair(FromSym, ToSym)] += Cnt;
    }
  }

  fout << "Fallthroughs" << std::endl;
  ExtraBBsIncludedInFallthroughs = 0;
  for (auto &FC : FallthroughCountersBySymbol) {
    std::vector<SymbolEntry *> Path;
    SymbolEntry *fallthroughFrom = FC.first.first,
                *fallthroughTo = FC.first.second;
    if (fallthroughFrom != fallthroughTo &&
        calculateFallthroughBBs(fallthroughFrom, fallthroughTo, Path)) {
      TotalCounters += (Path.size() + 1) * FC.second;
      for (auto *S : Path)
        ExtraBBsIncludedInFallthroughs += HotSymbols.insert(S).second ? 1 : 0;
    }

    fout << SymOrdinalF(*(FC.first.first)) << " "
         << SymOrdinalF(*(FC.first.second)) << " " << CountF(FC.second)
         << std::endl;
  }
  this->FallthroughsWritten = FallthroughCountersBySymbol.size();
}

template <class ELFT>
bool fillELFPhdr(llvm::object::ELFObjectFileBase *EBFile,
                 map<uint64_t, uint64_t> &PhdrLoadMap) {
  ELFObjectFile<ELFT> *eobj = dyn_cast<ELFObjectFile<ELFT>>(EBFile);
  if (!eobj) return false;
  const ELFFile<ELFT> *efile = eobj->getELFFile();
  if (!efile) return false;
  auto program_headers = efile->program_headers();
  if (!program_headers) return false;
  for (const typename ELFT::Phdr &phdr : *program_headers) {
    if (phdr.p_type == llvm::ELF::PT_LOAD &&
        (phdr.p_flags & llvm::ELF::PF_X) != 0) {
      auto E = PhdrLoadMap.find(phdr.p_vaddr);
      if (E == PhdrLoadMap.end()) {
        PhdrLoadMap.emplace(phdr.p_vaddr, phdr.p_memsz);
      } else {
        if (E->second != phdr.p_memsz) {
          LOG(ERROR) << "Invalid phdr found in elf binary file.";
          return false;
        }
      }
    }
  }
  if (PhdrLoadMap.empty()) {
    LOG(ERROR) << "No loadable and executable segments found in binary.";
    return false;
  }
  stringstream SS;
  SS << "Loadable and executable segments:\n";
  for (auto &Seg : PhdrLoadMap) {
    SS << "\tvaddr=" << hex0x << Seg.first << ", memsz=" << hex0x << Seg.second
       << std::endl;
  }
  LOG(INFO) << SS.str();
  return true;
}

bool PropellerProfWriter::initBinaryFile() {
  auto FileOrError = llvm::MemoryBuffer::getFile(BinaryFileName);
  if (!FileOrError) {
    LOG(ERROR) << "Failed to read file '" << BinaryFileName << "'.";
    return false;
  }
  this->BinaryFileContent = std::move(*FileOrError);

  auto ObjOrError = llvm::object::ObjectFile::createELFObjectFile(
      llvm::MemoryBufferRef(*(this->BinaryFileContent)));
  if (!ObjOrError) {
    LOG(ERROR) << "Not a valid ELF file '" << BinaryFileName << "'.";
    return false;
  }
  this->ObjFile = std::move(*ObjOrError);

  auto *ELFObjBase = dyn_cast<ELFObjectFileBase, ObjectFile>(ObjFile.get());
  BinaryIsPIE = (ELFObjBase->getEType() == llvm::ELF::ET_DYN);
  if (BinaryIsPIE) {
    const char *ELFIdent = BinaryFileContent->getBufferStart();
    const char ELFClass = ELFIdent[4];
    const char ELFData = ELFIdent[5];
    if (ELFClass == 1 && ELFData == 1) {
      fillELFPhdr<llvm::object::ELF32LE>(ELFObjBase, PhdrLoadMap);
    } else if (ELFClass == 1 && ELFData == 2) {
      fillELFPhdr<llvm::object::ELF32BE>(ELFObjBase, PhdrLoadMap);
    } else if (ELFClass == 2 && ELFData == 1) {
      fillELFPhdr<llvm::object::ELF64LE>(ELFObjBase, PhdrLoadMap);
    } else if (ELFClass == 2 && ELFData == 2) {
      fillELFPhdr<llvm::object::ELF64BE>(ELFObjBase, PhdrLoadMap);
    } else {
      assert(false);
    }
  }
  LOG(INFO) << "'" << this->BinaryFileName
            << "' is PIE binary: " << BinaryIsPIE;
  return true;
}

bool PropellerProfWriter::populateSymbolMap() {
  auto Symbols = ObjFile->symbols();
  const set<StringRef> ExcludedSymbols{"__cxx_global_array_dtor"};
  for (const auto &Sym : Symbols) {
    auto AddrR = Sym.getAddress();
    auto SecR = Sym.getSection();
    auto NameR = Sym.getName();
    auto TypeR = Sym.getType();

    if (!(AddrR && *AddrR && SecR && (*SecR)->isText() && NameR && TypeR))
      continue;

    StringRef Name = *NameR;
    if (Name.empty()) continue;
    uint64_t Addr = *AddrR;
    uint8_t Type(*TypeR);
    llvm::object::ELFSymbolRef ELFSym(Sym);
    uint64_t Size = ELFSym.getSize();

    StringRef BBFunctionName;
    bool isFunction = (Type == llvm::object::SymbolRef::ST_Function);
    bool isBB = SymbolEntry::isBBSymbol(Name, &BBFunctionName);

    if (!isFunction && !isBB) continue;
    if (isFunction && Size == 0) {
      // LOG(INFO) << "Dropped zero-sized function symbol '" << Name.str() <<
      // "'.";
      continue;
    }
    if (ExcludedSymbols.find(isBB ? BBFunctionName : Name) !=
        ExcludedSymbols.end()) {
      continue;
    }

    auto &L = AddrMap[Addr];
    if (!L.empty()) {
      // If we already have a symbol at the same address with same size, merge
      // them together.
      SymbolEntry *SymbolIsAliasedWith = nullptr;
      for (auto *S : L) {
        if (S->Size == Size) {
          // Make sure Name and Aliased name are both BB or both NON-BB.
          if (SymbolEntry::isBBSymbol(S->Name) !=
              SymbolEntry::isBBSymbol(Name)) {
            continue;
          }
          S->Aliases.push_back(Name);
          if (!S->isFunction() &&
              Type == llvm::object::SymbolRef::ST_Function) {
            // If any of the aliased symbols is a function, promote the whole
            // group to function.
            S->Type = llvm::object::SymbolRef::ST_Function;
          }
          SymbolIsAliasedWith = S;
          break;
        }
      }
      if (SymbolIsAliasedWith) continue;
    }

    // Delete symbol with same name from SymbolNameMap and AddrMap.
    map<StringRef, unique_ptr<SymbolEntry>>::iterator ExistingNameR =
        SymbolNameMap.find(Name);
    if (ExistingNameR != SymbolNameMap.end()) {
      LOG(INFO) << "Dropped duplicate symbol \""
                << SymNameF(*(ExistingNameR->second)) << "\". "
                << "Consider using \"-funique-internal-funcnames\" to "
                   "dedupe internal function names.";
      map<uint64_t, list<SymbolEntry *>>::iterator ExistingLI =
          AddrMap.find(ExistingNameR->second->Addr);
      if (ExistingLI != AddrMap.end()) {
        ExistingLI->second.remove_if(
            [&Name](SymbolEntry *S) { return S->Name == Name; });
      }
      SymbolNameMap.erase(ExistingNameR);
      continue;
    }

    SymbolEntry *NewSymbolEntry =
        new SymbolEntry(0, Name, SymbolEntry::AliasesTy(), Addr, Size, Type);
    L.push_back(NewSymbolEntry);
    NewSymbolEntry->BBTag = SymbolEntry::isBBSymbol(Name);
    SymbolNameMap.emplace(std::piecewise_construct, std::forward_as_tuple(Name),
                          std::forward_as_tuple(NewSymbolEntry));
  }  // End of iterating all symbols.

  // Now scan all the symbols in address order to create function <-> bb
  // relationship.
  uint64_t BBSymbolDropped = 0;
  decltype(AddrMap)::iterator LastFuncPos = AddrMap.end();
  for (auto P = AddrMap.begin(), Q = AddrMap.end(); P != Q; ++P) {
    int FuncCount = 0;
    for (auto *S : P->second) {
      if (S->isFunction() && !S->BBTag) {
        if (++FuncCount > 1) {
          // 2 different functions start at the same address, but with different
          // sizes, this is not supported.
          LOG(ERROR)
              << "Analyzing failure: at address 0x" << hex << P->first
              << ", there are more than 1 functions that have different sizes.";
          return false;
        }
        LastFuncPos = P;
      }
    }

    if (LastFuncPos == AddrMap.end()) continue;
    for (auto *S : P->second) {
      if (!S->BBTag) {
        // Set a function's wrapping function to itself.
        S->ContainingFunc = S;
        continue;
      }
      // This is a bb symbol, find a wrapping func for it.
      SymbolEntry *ContainingFunc = nullptr;
      for (SymbolEntry *FP : LastFuncPos->second) {
        if (FP->isFunction() && !FP->BBTag && FP->containsAnotherSymbol(S) &&
            FP->isFunctionForBBName(S->Name)) {
          if (ContainingFunc == nullptr) {
            ContainingFunc = FP;
          } else {
            // Already has a containing function, so we have at least 2
            // different functions with different sizes but start at the same
            // address, impossible?
            LOG(ERROR) << "Analyzing failure: at address 0x" << hex
                       << LastFuncPos->first
                       << ", there are 2 different functions: "
                       << SymNameF(ContainingFunc) << " and "
                       << SymNameF(FP);
            return false;
          }
        }
      }
      if (!ContainingFunc) {
        // Disambiguate the following case:
        // 0x10 foo       size = 2
        // 0x12 foo.bb.1  size = 2
        // 0x14 foo.bb.2  size = 0
        // 0x14 bar  <- LastFuncPos set is to bar.
        // 0x14 bar.bb.1
        // In this scenario, we seek lower address.
        auto T = LastFuncPos;
        int FunctionSymbolSeen = 0;
        while (T != AddrMap.begin()) {
          T = std::prev(T);
          bool isFunction = false;
          for (auto *KS : T->second) {
            isFunction |= KS->isFunction();
            if (KS->isFunction() && !KS->BBTag &&
                KS->containsAnotherSymbol(S) &&
                KS->isFunctionForBBName(S->Name)) {
              ContainingFunc = KS;
              break;
            }
          }
          FunctionSymbolSeen += isFunction ? 1 : 0;
          // Only go back for at most 2 function symbols.
          if (FunctionSymbolSeen > 2) break;
        }
      }
      S->ContainingFunc = ContainingFunc;
      if (S->ContainingFunc == nullptr) {
        LOG(ERROR) << "Dropped bb symbol without any wrapping function: \""
                   << SymShortF(S) << "\"";
        ++BBSymbolDropped;
        AddrMap.erase(P--);
        break;
      } else {
        if (!ContainingFunc->isFunctionForBBName(S->Name)) {
          LOG(ERROR) << "Internal check warning: \n"
                     << "Sym: " << SymShortF(S) << "\n"
                     << "Func: " << SymShortF(S->ContainingFunc);
          return false;
        }
      }
      
      // Now here is the tricky thing to fix:
      //    Wrapping func _zfooc2/_zfooc1/_zfooc3
      //    bbname: a.BB._zfooc1
      //
      // We want to make sure the primary name (the name first appears in the
      // alias) matches the bb name, so we change the wrapping func aliases to:
      //    _zfooc1/_zfooc2/_zfooc3
      // By doing this, the wrapping func matches "a.BB._zfooc1" correctly.
      //
      if (!ContainingFunc->Aliases.empty()) {
        auto A = S->Name.split(lld::propeller::BASIC_BLOCK_SEPARATOR);
        auto ExpectFuncName = A.second;
        auto &Aliases = ContainingFunc->Aliases;
        if (ExpectFuncName != ContainingFunc->Name) {
          SymbolEntry::AliasesTy::iterator P, Q;
          for (P = Aliases.begin(), Q = Aliases.end(); P != Q; ++P)
            if (*P == ExpectFuncName) break;

          if (P == Q) {
            LOG(ERROR) << "Internal check error: bb symbol '" << S->Name.str()
                       << "' does not have a valid wrapping function.";
            return false;
          }
          StringRef OldName = ContainingFunc->Name;
          ContainingFunc->Name = *P;
          Aliases.erase(P);
          Aliases.push_back(OldName);
        }
      }

      // Replace the whole name (e.g. "aaaa.BB.foo" with "aaaa" only);
      StringRef FName, BName;
      bool R = SymbolEntry::isBBSymbol(S->Name, &FName, &BName);
      (void)(R);
      assert(R);
      if (FName != S->ContainingFunc->Name) {
        LOG(ERROR) << "Internal check error: bb symbol '" << S->Name.str()
                       << "' does not have a valid wrapping function.";
      }
      S->Name = BName;
    }  // End of iterating P->second
  }    // End of iterating AddrMap.
  if (BBSymbolDropped)
    LOG(INFO) << "Dropped " << dec << CommaF(BBSymbolDropped)
              << " bb symbol(s).";
  return true;
}

bool PropellerProfWriter::parsePerfData() {
  this->PerfDataFileParsed = 0;
  StringRef FN(PerfFileName);
  while (!FN.empty()) {
    StringRef PerfName;
    std::tie(PerfName, FN) = FN.split(',');
    if (!parsePerfData(PerfName.str())) {
      return false;
    }
    ++this->PerfDataFileParsed;
  }
  LOG(INFO) << "Processed " << PerfDataFileParsed << " perf file(s).";
  return true;
}

bool PropellerProfWriter::parsePerfData(const string &PName) {
  quipper::PerfReader PR;
  if (!PR.ReadFile(PName)) {
    LOG(ERROR) << "Failed to read perf data file: " << PName;
    return false;
  }

  quipper::PerfParser Parser(&PR);
  if (!Parser.ParseRawEvents()) {
    LOG(ERROR) << "Failed to parse perf raw events for perf file: '" << PName
               << "'.";
    return false;
  }

  if (!FLAGS_ignore_build_id) {
    if (!setupBinaryMMapName(PR, PName)) {
      return false;
    }
  }

  if (!setupMMaps(Parser, PName)) {
    LOG(ERROR) << "Failed to find perf mmaps for binary '" << BinaryFileName
               << "'.";
    return false;
  }

  return aggregateLBR(Parser);
}

bool PropellerProfWriter::setupMMaps(quipper::PerfParser &Parser,
                                     const string &PName) {
  // Depends on the binary file name, if
  //   - it is absolute, compares it agains the full path
  //   - it is relative, only compares the file name part
  // Note: CompFunc is constructed in a way so that there is no branch /
  // conditional test inside the function.
  struct BinaryNameComparator {
    BinaryNameComparator(const string &BinaryFileName) {
      if (llvm::sys::path::is_absolute(BinaryFileName)) {
        ComparePart = StringRef(BinaryFileName);
        PathChanger = NullPathChanger;
      } else {
        ComparePart = llvm::sys::path::filename(BinaryFileName);
        PathChanger = NameOnlyPathChanger;
      }
    }

    bool operator()(const string &Path) {
      return ComparePart == PathChanger(Path);
    }

    StringRef ComparePart;
    std::function<StringRef(const string &)> PathChanger;

    static StringRef NullPathChanger(const string &S) { return StringRef(S); }
    static StringRef NameOnlyPathChanger(const string &S) {
      return llvm::sys::path::filename(StringRef(S));
    }
  } CompFunc(FLAGS_match_mmap_file.empty()
                 ? (this->BinaryMMapName.empty() ? BinaryFileName
                                                 : this->BinaryMMapName)
                 : FLAGS_match_mmap_file);

  for (const auto &PE : Parser.parsed_events()) {
    quipper::PerfDataProto_PerfEvent *EPtr = PE.event_ptr;
    if (EPtr->event_type_case() != quipper::PerfDataProto_PerfEvent::kMmapEvent)
      continue;

    const quipper::PerfDataProto_MMapEvent &MMap = EPtr->mmap_event();
    if (!MMap.has_filename()) continue;

    const string &MMapFileName = MMap.filename();
    if (!CompFunc(MMapFileName) || !MMap.has_start() || !MMap.has_len() ||
        !MMap.has_pid())
      continue;

    if (this->BinaryMMapName.empty()) {
      this->BinaryMMapName = MMapFileName;
    } else if (BinaryMMapName != MMapFileName) {
      LOG(ERROR) << "'" << BinaryFileName
                 << "' is not specific enough. It matches both '"
                 << BinaryMMapName << "' and '" << MMapFileName
                 << "' in the perf data file '" << PName
                 << "'. Consider using absolute file name.";
      return false;
    }
    uint64_t LoadAddr = MMap.start();
    uint64_t LoadSize = MMap.len();
    uint64_t PageOffset = MMap.has_pgoff() ? MMap.pgoff() : 0;

    // For the same binary, MMap can only be different if it is a PIE binary. So
    // for non-PIE binaries, we check all MMaps are equal and merge them into
    // BinaryMMapByPid[0].
    uint64_t MPid = BinaryIsPIE ? MMap.pid() : 0;
    set<MMapEntry> &LoadMap = BinaryMMapByPid[MPid];
    // Check for mmap conflicts.
    if (!checkBinaryMMapConflictionAndEmplace(LoadAddr, LoadSize, PageOffset,
                                              LoadMap)) {
      stringstream SS;
      SS << "Found conflict MMap event: "
         << MMapEntry{LoadAddr, LoadSize, PageOffset}
         << ". Existing MMap entries: " << std::endl;
      for (auto &EM : LoadMap) {
        SS << "\t" << EM << std::endl;
      }
      LOG(ERROR) << SS.str();
      return false;
    }
  }  // End of iterating mmap events.

  if (!std::accumulate(
          BinaryMMapByPid.begin(), BinaryMMapByPid.end(), 0,
          [](uint64_t V, const decltype(BinaryMMapByPid)::value_type &S)
              -> uint64_t { return V + S.second.size(); })) {
    LOG(ERROR) << "Failed to find MMap entries in '" << PName << "' for '"
               << BinaryFileName << "'.";
    return false;
  }
  for (auto &M : BinaryMMapByPid) {
    stringstream SS;
    SS << "Found mmap in '" << PName << "' for binary: '" << BinaryFileName
       << "', pid=" << dec << M.first << " (0 for non-pie executables)"
       << std::endl;
    for (auto &N : M.second) {
      SS << "\t" << N << std::endl;
    }
    LOG(INFO) << SS.str();
  }
  return true;
}

bool PropellerProfWriter::checkBinaryMMapConflictionAndEmplace(
    uint64_t LoadAddr, uint64_t LoadSize, uint64_t PageOffset,
    set<MMapEntry> &M) {
  for (const MMapEntry &E : M) {
    if (E.LoadAddr == LoadAddr && E.LoadSize == LoadSize &&
        E.PageOffset == PageOffset)
      return true;
    if (!((LoadAddr + LoadSize <= E.LoadAddr) ||
          (E.LoadAddr + E.LoadSize <= LoadAddr)))
      return false;
  }
  auto R = M.emplace(LoadAddr, LoadSize, PageOffset);
  assert(R.second);
  return true;
}

bool PropellerProfWriter::setupBinaryMMapName(quipper::PerfReader &PR,
                                              const string &PName) {
  this->BinaryMMapName = "";
  if (FLAGS_ignore_build_id || this->BinaryBuildId.empty()) {
    return true;
  }
  list<pair<string, string>> ExistingBuildIds;
  for (const auto &BuildId : PR.build_ids()) {
    if (BuildId.has_filename() && BuildId.has_build_id_hash()) {
      string PerfBuildId = BuildId.build_id_hash();
      quipper::PerfizeBuildIDString(&PerfBuildId);
      ExistingBuildIds.emplace_back(BuildId.filename(), PerfBuildId);
      if (PerfBuildId == this->BinaryBuildId) {
        this->BinaryMMapName = BuildId.filename();
        LOG(INFO) << "Found file with matching BuildId in perf file '" << PName
                  << "': " << this->BinaryMMapName;
        return true;
      }
    }
  }
  stringstream SS;
  SS << "No file with matching BuildId in perf data '" << PName
     << "', which contains the following <file, buildid>:" << std::endl;
  for (auto &P : ExistingBuildIds) {
    SS << "\t" << P.first << ": " << BuildIdWrapper(P.second.c_str())
       << std::endl;
  }
  LOG(INFO) << SS.str();
  return false;
}

bool PropellerProfWriter::aggregateLBR(quipper::PerfParser &Parser) {
  uint64_t brstackCount = 0;
  for (const auto &PE : Parser.parsed_events()) {
    quipper::PerfDataProto_PerfEvent *EPtr = PE.event_ptr;
    if (EPtr->event_type_case() ==
        quipper::PerfDataProto_PerfEvent::kSampleEvent) {
      auto &SEvent = EPtr->sample_event();
      if (!SEvent.has_pid()) continue;
      auto BRStack = SEvent.branch_stack();
      if (BRStack.empty()) continue;
      uint64_t Pid = BinaryIsPIE ? SEvent.pid() : 0;
      if (BinaryMMapByPid.find(Pid) == BinaryMMapByPid.end()) continue;
      auto &BranchCounters = BranchCountersByPid[Pid];
      auto &FallthroughCounters = FallthroughCountersByPid[Pid];
      uint64_t LastFrom = INVALID_ADDRESS;
      uint64_t LastTo = INVALID_ADDRESS;
      brstackCount += BRStack.size();
      for (int P = BRStack.size() - 1; P >= 0; --P) {
        const auto &BE = BRStack.Get(P);
        uint64_t From = BE.from_ip();
        uint64_t To = BE.to_ip();
        if (P == 0 && From == LastFrom && To == LastTo) {
          // LOG(INFO) << "Ignoring duplicate LBR entry: 0x" << std::hex << From
          //           << "-> 0x" << To << std::dec << "\n";
          continue;
        }
        ++(BranchCounters[make_pair(From, To)]);
        if (LastTo != INVALID_ADDRESS && LastTo <= From)
          ++(FallthroughCounters[make_pair(LastTo, From)]);
        LastTo = To;
        LastFrom = From;
      }
    }
  }
  if (brstackCount < 100) {
    LOG(ERROR) << "Too few brstack records (only " << brstackCount
               << " record(s) found), cannot continue.";
    return false;
  }
  LOG(INFO) << "Processed " << CommaF(brstackCount) << " lbr records.";
  return true;
}

bool PropellerProfWriter::findBinaryBuildId() {
  this->BinaryBuildId = "";
  if (FLAGS_ignore_build_id) return true;
  bool BuildIdFound = false;
  for (auto &SR : ObjFile->sections()) {
    llvm::object::ELFSectionRef ESR(SR);
    StringRef SName;
    auto ExSecRefName = SR.getName();
    if (ExSecRefName) {
      SName = *ExSecRefName;
    } else {
      continue;
    }
    auto ExpectedSContents = SR.getContents();
    if (ESR.getType() == llvm::ELF::SHT_NOTE && SName == ".note.gnu.build-id" &&
        ExpectedSContents && !ExpectedSContents->empty()) {
      StringRef SContents = *ExpectedSContents;
      const unsigned char *P = SContents.bytes_begin() + 0x10;
      if (P >= SContents.bytes_end()) {
        LOG(INFO) << "Section '.note.gnu.build-id' does not contain valid "
                     "build id information.";
        return true;
      }
      string BuildId((const char *)P, SContents.size() - 0x10);
      quipper::PerfizeBuildIDString(&BuildId);
      this->BinaryBuildId = BuildId;
      LOG(INFO) << "Found Build Id in binary '" << BinaryFileName
                << "': " << BuildIdWrapper(BuildId.c_str());
      return true;
    }
  }
  LOG(INFO) << "No Build Id found in '" << BinaryFileName << "'.";
  return true;  // always returns true
}

#endif
