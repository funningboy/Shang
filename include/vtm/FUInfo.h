//===------- VFunctionUnit.h - VTM Function Unit Information ----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains define the function unit class in Verilog target machine.
//
//===----------------------------------------------------------------------===//

#ifndef VTM_FUNCTION_UNIT_H
#define VTM_FUNCTION_UNIT_H
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

namespace luabind {
  namespace adl {
    class object;
  }
  using adl::object;
}

namespace llvm {
class TargetRegisterClass;

namespace VFUs {
  enum FUTypes {
    Trivial = 0,
    AddSub = 1,
    Shift = 2,
    Mult = 3,
    MemoryBus = 4,
    BRam = 5,

    FirstFUType = Trivial,
    FirstNonTrivialFUType = AddSub,
    LastPostBindFUType = Mult,
    NumPostBindFUs = LastPostBindFUType - FirstNonTrivialFUType + 1,
    LastCommonFUType = BRam,
    NumCommonFUs = LastCommonFUType - FirstFUType + 1,
    NumNonTrivialCommonFUs = LastCommonFUType - FirstNonTrivialFUType + 1,
    // Special function unit.
    // RTL module corresponding to callee functions of function corresponding to
    // current RTL module.
    CalleeFN = 6,
    // Finite state machine finish.
    FSMFinish = 7,
    LastFUType = FSMFinish,
    NumFUs = LastFUType - FirstFUType + 1,
    // Helper enumeration value, just for internal use as a flag to indicate
    // all kind of function units are selected.
    AllFUType = 0xf
  };

  extern const char *VFUNames[];
  const TargetRegisterClass *getRepRegisterClass(enum FUTypes T);
}

class FuncUnitId {
  union {
    struct {
      VFUs::FUTypes Type  : 4;
      unsigned  Num : 12;
    } ID;

    uint16_t data;
  } UID;

public:
  // The general FUId of a given type.
  inline explicit FuncUnitId(VFUs::FUTypes T, unsigned N) {
    UID.ID.Type = T;
    UID.ID.Num = N;
  }

  /*implicit*/ FuncUnitId(VFUs::FUTypes T = VFUs::Trivial) {
    UID.ID.Type = T;
    UID.ID.Num = (T == VFUs::FSMFinish) ? 0 : 0xfff;
  }

  explicit FuncUnitId(uint16_t Data) {
    UID.data = Data;
  }

  inline VFUs::FUTypes getFUType() const { return UID.ID.Type; }
  inline unsigned getFUNum() const { return UID.ID.Num; }
  inline unsigned getData() const { return UID.data; }
  inline bool isUnknownInstance() const { return getFUNum() == 0xfff; }

  inline bool isTrivial() const { return getFUType() == VFUs::Trivial; }
  inline bool isBound() const {
    return !isTrivial() && getFUNum() != 0xfff;
  }

  // Get the total avaliable number of this kind of function unit.
  unsigned getTotalFUs() const;

  inline bool operator==(const FuncUnitId X) const { return UID.data == X.UID.data; }
  inline bool operator!=(const FuncUnitId X) const { return !operator==(X); }
  inline bool operator< (const FuncUnitId X) const { return UID.data < X.UID.data; }

  void print(raw_ostream &OS) const;
  void dump() const;
};

inline static raw_ostream &operator<<(raw_ostream &O, const FuncUnitId &ID) {
  ID.print(O);
  return O;
}

/// @brief The description of Verilog target machine function units.
class VFUDesc {
  VFUDesc(const VFUDesc &);            // DO NOT IMPLEMENT
  void operator=(const VFUDesc &);  // DO NOT IMPLEMENT
protected:
  // The HWResource baseclass this node corresponds to
  const unsigned ResourceType;
  // How many cycles to finish?
  const unsigned Latency;
  // Start interval
  const unsigned StartInt;
  // How many resources available?
  const unsigned TotalRes;
  // The MaxBitWidth of the function unit.
  const unsigned MaxBitWidth;

  VFUDesc(VFUs::FUTypes type, unsigned latency, unsigned startInt,
          unsigned totalRes, unsigned maxBitWidth)
    : ResourceType(type), Latency(latency), StartInt(startInt),
    TotalRes(totalRes), MaxBitWidth(maxBitWidth) {}
public:
  VFUDesc(VFUs::FUTypes type, luabind::object FUTable);

  static const char *getTypeName(VFUs::FUTypes FU) {
    return VFUs::VFUNames[FU];
  }

  unsigned getType() const { return ResourceType; }
  const char *getTypeName() const {
    return getTypeName((VFUs::FUTypes)getType());
  }

  unsigned getLatency() const { return Latency; }
  unsigned getTotalRes() const { return TotalRes; }
  unsigned getStartInt() const { return StartInt; }
  unsigned getMaxBitWidth() const { return MaxBitWidth; }

  virtual void print(raw_ostream &OS) const;
};

class VFUMemBus : public VFUDesc {
  unsigned AddrWidth;

public:
  VFUMemBus(luabind::object FUTable);

  unsigned getAddrWidth() const { return AddrWidth; }
  unsigned getDataWidth() const { return getMaxBitWidth(); }

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VFUMemBus *A) { return true; }
  static inline bool classof(const VFUDesc *A) {
    return A->getType() == VFUs::MemoryBus;
  }

  static VFUs::FUTypes getType() { return VFUs::MemoryBus; }
  static const char *getTypeName() { return VFUs::VFUNames[getType()]; }

  // Signal names of the function unit.
  inline static std::string getAddrBusName(unsigned FUNum) {
    return "mem" + utostr(FUNum) + "addr";
  }

  inline static std::string getInDataBusName(unsigned FUNum) {
    return "mem" + utostr(FUNum) + "in";
  }

  inline static std::string getOutDataBusName(unsigned FUNum) {
    return "mem" + utostr(FUNum) + "out";
  }

  // Dirty Hack: This should be byte enable.
  inline static std::string getByteEnableName(unsigned FUNum) {
    return "mem" + utostr(FUNum) + "be";
  }

  inline static std::string getWriteEnableName(unsigned FUNum) {
    return "mem" + utostr(FUNum) + "we";
  }

  inline static std::string getEnableName(unsigned FUNum) {
    return "mem" + utostr(FUNum) + "en";
  }

  inline static std::string getReadyName(unsigned FUNum) {
    return "mem" + utostr(FUNum) + "rdy";
  }
};

template<enum VFUs::FUTypes T>
class VSimpleFUDesc : public VFUDesc {
public:
  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  template<enum VFUs::FUTypes OtherT>
  static inline bool classof(const VSimpleFUDesc<OtherT> *A) {
    return T == OtherT;
  }
  static inline bool classof(const VFUDesc *A) {
    return A->getType() == T;
  }

  static VFUs::FUTypes getType() { return T; };
  static const char *getTypeName() { return VFUs::VFUNames[getType()]; }
};

typedef VSimpleFUDesc<VFUs::AddSub>  VFUAddSub;
typedef VSimpleFUDesc<VFUs::Shift>   VFUShift;
typedef VSimpleFUDesc<VFUs::Mult>    VFUMult;

class VFUBRam : public  VFUDesc {
  std::string Template; // Template for inferring block ram.
public:
  VFUBRam(luabind::object FUTable);

  std::string generateCode(const std::string &Clk, unsigned Num,
                           unsigned DataWidth, unsigned AddrWidth) const;

  static inline bool classof(const VFUBRam *A) {
    return true;
  }

  template<enum VFUs::FUTypes OtherT>
  static inline bool classof(const VSimpleFUDesc<OtherT> *A) {
    return getType() == OtherT;
  }

  static inline bool classof(const VFUDesc *A) {
    return A->getType() == VFUs::BRam;
  }

  static VFUs::FUTypes getType() { return VFUs::BRam; };
  static const char *getTypeName() { return VFUs::VFUNames[getType()]; }

  // Signal names of the function unit.
  inline static std::string getAddrBusName(unsigned FUNum) {
    return "bram" + utostr(FUNum) + "addr";
  }

  inline static std::string getInDataBusName(unsigned FUNum) {
    return "bram" + utostr(FUNum) + "in";
  }

  inline static std::string getOutDataBusName(unsigned FUNum) {
    return "bram" + utostr(FUNum) + "out";
  }

  // Dirty Hack: This should be byte enable.
  inline static std::string getByteEnableName(unsigned FUNum) {
    return "bram" + utostr(FUNum) + "be";
  }

  inline static std::string getWriteEnableName(unsigned FUNum) {
    return "bram" + utostr(FUNum) + "we";
  }

  inline static std::string getEnableName(unsigned FUNum) {
    return "bram" + utostr(FUNum) + "en";
  }
};

struct CommonFUIdentityFunctor
  : public std::unary_function<enum VFUs::FUTypes, unsigned>{

  unsigned operator()(enum VFUs::FUTypes T) const {
    return (unsigned)T - (unsigned)VFUs::FirstFUType;
  }
};

VFUDesc *getFUDesc(enum VFUs::FUTypes T);

template<class ResType>
ResType *getFUDesc() {
  return cast<ResType>(getFUDesc(ResType::getType()));
}
}

#endif
