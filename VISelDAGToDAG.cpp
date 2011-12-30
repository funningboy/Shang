//===----------- VDAGToDAG.cpp - A dag to dag inst selector for VTM -------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines an instruction selector for the VTM target.
//
//===----------------------------------------------------------------------===//

#include "VTargetMachine.h"
#include "vtm/FUInfo.h"
#include "vtm/Utilities.h"

#include "llvm/Intrinsics.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm/Target/TargetData.h"

using namespace llvm;

//===----------------------------------------------------------------------===//
// Instruction Selector Implementation
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
/// VTMDAGToDAGISel - VTM specific code to select VTM instructions for
/// SelectionDAG operations.
namespace {
class VDAGToDAGISel : public SelectionDAGISel {
public:
  VDAGToDAGISel(VTargetMachine &TM, CodeGenOpt::Level OptLevel)
    : SelectionDAGISel(TM, OptLevel) {}

  virtual const char *getPassName() const {
    return "VTM DAG->DAG Pattern Instruction Selection";
  }

  // Include the pieces autogenerated from the target description.
#include "VGenDAGISel.inc"

private:
  // The last operand of all VTM machine instructions is the bit width operand,
  // which hold the bit width information of all others operands. This operand
  // is a 64 bit immediate.
  BitWidthAnnotator computeOperandsBitWidth(SDNode *N, SDValue Ops[],
                                            unsigned NumOps);

  // Copy constant to function unit operand register explicitly
  bool shouldMoveToReg(SDNode *N) {
    return isa<ConstantSDNode>(N) || isa<ExternalSymbolSDNode>(N);
  }

  SDNode *Select(SDNode *N);

  SDNode *SelectConstBitSlice(ConstantSDNode *CSD, SDNode *N);
  SDNode *SelectBitSlice(SDNode *N);

  SDNode *SelectUnary(SDNode *N, unsigned OpC);
  // If we need to copy the operand to register explicitly, set CopyOp to true.
  SDNode *SelectBinary(SDNode *N, unsigned OpC);
  SDNode *SelectSimpleNode(SDNode *N, unsigned OpC);

  // Function argument and return values.
  SDNode *SelectInternalCall(SDNode *N);
  SDNode *SelectLoadArgument(SDNode *N);
  SDNode *SelectRetVal(SDNode *N);
  SDNode *SelectBrcnd(SDNode *N);

  SDNode *buildBitSlice(SDNode *N, unsigned SizeOfN, unsigned UB, unsigned LB);

  SDNode *SelectImmediate(SDNode *N, bool ForceMove = false);

  SDNode *SelectMemAccess(SDNode *N);
  SDNode *SelectBRamAccess(SDNode *N);

  SDNode *SelectINTRINSIC_W_CHAIN(SDNode *N);

  virtual void PreprocessISelDAG();
  virtual void PostprocessISelDAG();
  void CopyToReg(SelectionDAG &DAG, SDNode *N);

  const VInstrInfo &getInstrInfo() {
    return *static_cast<const VTargetMachine&>(TM).getInstrInfo();
  }
  const VRegisterInfo *getRegisterInfo() {
    return static_cast<const VTargetMachine&>(TM).getRegisterInfo();
  }

  void LowerMemAccessISel(SDNode *N, SelectionDAG &DAG, bool isStore);
  void LowerADDEForISel(SDNode *N, SelectionDAG &DAG);
  void LowerUMUL_LOHIForISel(SDNode *N, SelectionDAG &DAG);
  // Return true if the node already lowered.
  void LowerICmpForISel(SDNode *N, SelectionDAG &DAG);

};
}  // end anonymous namespace

FunctionPass *llvm::createVISelDag(VTargetMachine &TM,
                                   CodeGenOpt::Level OptLevel) {
  return new VDAGToDAGISel(TM, OptLevel);
}

BitWidthAnnotator VDAGToDAGISel::computeOperandsBitWidth(SDNode *N,
                                                         SDValue Ops[],
                                                         unsigned NumOps) {
  BitWidthAnnotator Annotator;
  unsigned NumDefs = 0;
  // Skip the trace number.
  NumOps -= 1;

  for (unsigned i = 0; i < N->getNumValues(); ++i) {
    // Since chains will not appear in the machine instructions, we need to skip
    // them.
    if (N->getValueType(i) == MVT::Other)
      continue;

    Annotator.setBitWidth(VTargetLowering::computeSizeInBits(SDValue(N, i)), NumDefs);
    ++NumDefs;
  }

  // Set up the operand widths.
  unsigned MaxOps = std::min(NumOps - 1, BitWidthAnnotator::size() - NumDefs);
  for (unsigned i = 0; i < MaxOps; ++i) {
    if (Ops[i].getValueType() == MVT::Other) continue;

    Annotator.setBitWidth(VTargetLowering::computeSizeInBits(Ops[i]),
                          i + NumDefs); // Skip the chains.
  }

  // FIXME: Build the bit width information.
  Ops[NumOps -1] = CurDAG->getTargetConstant(Annotator.get(), MVT::i64);
  return Annotator;
}

SDNode *VDAGToDAGISel::SelectUnary(SDNode *N, unsigned OpC) {
  SDValue Ops [] = { N->getOperand(0),
                     SDValue()/*The dummy bit width operand*/,
                     CurDAG->getTargetConstant(0, MVT::i64) /*and trace number*/
                   };

  computeOperandsBitWidth(N, Ops, array_lengthof(Ops));

  return CurDAG->SelectNodeTo(N, OpC, N->getVTList(),
                              Ops, array_lengthof(Ops));
}

template<std::size_t N>
static inline void updateBitWidthAnnotator(SDValue (&Ops)[N], SelectionDAG *DAG,
  int64_t BitWidthInfo) {
    *(array_endof(Ops) - 2) = DAG->getTargetConstant(BitWidthInfo, MVT::i64);
}

SDNode *VDAGToDAGISel::SelectBinary(SDNode *N, unsigned OpC) {
  // Copy immediate to register if necessary.
  SDValue Ops [] = { N->getOperand(0),
                     N->getOperand(1),
                     SDValue()/*The dummy bit width operand*/,
                     CurDAG->getTargetConstant(0, MVT::i64) /*and trace number*/
                   };

  computeOperandsBitWidth(N, Ops, array_lengthof(Ops));

  return CurDAG->SelectNodeTo(N, OpC, N->getVTList(),
                              Ops, array_lengthof(Ops));
}

SDNode *VDAGToDAGISel::buildBitSlice(SDNode *N, unsigned SizeOfN,
                                     unsigned UB, unsigned LB) {
  BitWidthAnnotator Annotator;
  Annotator.setBitWidth(UB - LB, 0);
  Annotator.setBitWidth(SizeOfN, 1);
  Annotator.setBitWidth(8, 2);
  Annotator.setBitWidth(8, 3);
  SDValue Ops[] = { SDValue(N, 0),
    // UB
    CurDAG->getTargetConstant(UB, MVT::i8),
    // LB
    CurDAG->getTargetConstant(LB, MVT::i8),
    // Bitwidth operand
    CurDAG->getTargetConstant(Annotator.get(), MVT::i64),
    // Trace number
    CurDAG->getTargetConstant(0, MVT::i64)
  };

  EVT VT = VTargetLowering::getRoundIntegerOrBitType(UB - LB,
                                                     *CurDAG->getContext());
  return CurDAG->getMachineNode(VTM::VOpBitSlice, N->getDebugLoc(),
                                VT, Ops, array_lengthof(Ops));
}

SDNode *VDAGToDAGISel::SelectSimpleNode(SDNode *N, unsigned Opc) {
  SmallVector<SDValue, 4> Ops;
  for (SDNode::op_iterator I = N->op_begin(), E = N->op_end(); I != E; ++I)
    Ops.push_back(*I);

  Ops.push_back(SDValue());//The dummy bit width operand
  Ops.push_back(CurDAG->getTargetConstant(0, MVT::i64)); /*and trace number*/

  computeOperandsBitWidth(N, Ops.data(), Ops.size());

  return CurDAG->SelectNodeTo(N, Opc, N->getVTList(), Ops.data(), Ops.size());
}

SDNode *VDAGToDAGISel::SelectConstBitSlice(ConstantSDNode *CSD, SDNode *N) {
  int64_t val = CSD->getSExtValue();
  unsigned UB = N->getConstantOperandVal(1);
  unsigned LB = N->getConstantOperandVal(2);
  val = getBitSlice64(val, UB, LB);
  EVT VT = EVT::getIntegerVT(*CurDAG->getContext(), UB - LB);
  SDValue C = CurDAG->getTargetConstant(val, VT);
  // Copy the constant explicit since the value may use by some function unit.
  SDValue Ops[] = { C, SDValue()/*The dummy bit width operand*/,
                    CurDAG->getTargetConstant(0, MVT::i64) /*and trace number*/
                  };
  computeOperandsBitWidth(C.getNode(), Ops, array_lengthof(Ops));
  return CurDAG->SelectNodeTo(N, VTM::VOpMove_ri, N->getValueType(0),
                              Ops, array_lengthof(Ops));
}

SDNode *VDAGToDAGISel::SelectBitSlice(SDNode *N) {
  SDNode *Op = N->getOperand(0).getNode();
  // Emit the constant bit slice to constant directly if possible.
  if (ConstantSDNode *CSD = dyn_cast<ConstantSDNode>(Op))
    return SelectConstBitSlice(CSD, N);

  assert(Op->getOpcode() != VTMISD::BitSlice
         && (!Op->isMachineOpcode()
             || Op->getMachineOpcode() != VTM::VOpBitSlice)
         && "DAGCombine should handle this!");

  return SelectSimpleNode(N, VTM::VOpBitSlice);
}

SDNode *VDAGToDAGISel::SelectImmediate(SDNode *N, bool ForceMove) {
  SDValue Imm = SDValue(N, 0);
  DebugLoc dl = Imm.getDebugLoc();

  if (ConstantSDNode *CSD = dyn_cast<ConstantSDNode>(N)) {
    // Do not need to select target constant.
    if (CSD->getOpcode() == ISD::TargetConstant && !ForceMove)
      return 0;

    // FIXME: We do not need this since we have the bit width operand to hold
    // the bit width of a constant.
    // Build the target constant.
    int64_t Val = CSD->getZExtValue();
    Imm = CurDAG->getTargetConstant(Val, N->getValueType(0));
  } else if (ExternalSymbolSDNode *ES = dyn_cast<ExternalSymbolSDNode>(N))
    Imm = CurDAG->getTargetExternalSymbol(ES->getSymbol(), Imm.getValueType(),
                                          Imm.getValueSizeInBits());
  else {
    GlobalAddressSDNode *GA = cast<GlobalAddressSDNode>(N);
    Imm = CurDAG->getTargetGlobalAddress(GA->getGlobal(), dl,
                                          Imm.getValueType(), GA->getOffset(),
                                          Imm.getValueSizeInBits());
  }

  SDValue Ops[] = { Imm, SDValue()/*The dummy bit width operand*/,
                    CurDAG->getTargetConstant(0, MVT::i64) /*and trace number*/
                  };

  computeOperandsBitWidth(N, Ops, array_lengthof(Ops));

  // Do not create cycle.
  if (ForceMove)
    return CurDAG->getMachineNode(VTM::VOpMove_ri, dl, N->getVTList(),
                                  Ops, array_lengthof(Ops));
  else
    return CurDAG->SelectNodeTo(N, VTM::VOpMove_ri, N->getVTList(),
                                Ops, array_lengthof(Ops));
}

SDNode *VDAGToDAGISel::SelectBrcnd(SDNode *N) {
  SDValue TargetBB = N->getOpcode() == ISD::BR ? N->getOperand(1)
                                               : N->getOperand(2);
  SDValue Cnd = N->getOpcode() == ISD::BR ? CurDAG->getTargetConstant(1,MVT::i1)
                                          : N->getOperand(1);
  SDValue Ops[] = { Cnd, // Condition
                    TargetBB, // Target BB
                    SDValue(),
                    CurDAG->getTargetConstant(0, MVT::i64), /*and trace number*/
                    N->getOperand(0) }; // Chain

  computeOperandsBitWidth(N, Ops, array_lengthof(Ops) - 1);

  return CurDAG->SelectNodeTo(N, VTM::VOpToStateb, N->getVTList(),
                              Ops, array_lengthof(Ops));
}

SDNode *VDAGToDAGISel::SelectInternalCall(SDNode *N) {
  SmallVector<SDValue, 8> Ops;

  for (unsigned I = 1, E = N->getNumOperands(); I != E; ++I)
    Ops.push_back(N->getOperand(I));
  Ops.push_back(SDValue()); // The bit width annotator.
  Ops.push_back(CurDAG->getTargetConstant(0, MVT::i64)); /*and trace number*/
  // And the chain.
  Ops.push_back(N->getOperand(0));

  computeOperandsBitWidth(N, Ops.data(), Ops.size() -1/*Skip the chain*/);

  return CurDAG->SelectNodeTo(N, VTM::VOpInternalCall, N->getVTList(),
                              Ops.data(), Ops.size());
}

SDNode *VDAGToDAGISel::SelectLoadArgument(SDNode *N) {
  SDValue Ops[] = { N->getOperand(1),
                    SDValue()/*The dummy bit width operand*/,
                    CurDAG->getTargetConstant(0, MVT::i64) /*and trace number*/,
                    N->getOperand(0) };

  computeOperandsBitWidth(N, Ops, array_lengthof(Ops) -1 /*Skip the chain*/);

  return CurDAG->SelectNodeTo(N, VTM::VOpMove_rw, N->getVTList(),
                              Ops, array_lengthof(Ops));
}

SDNode *VDAGToDAGISel::SelectRetVal(SDNode *N) {
  SDValue RetValIdx = N->getOperand(2);
  int64_t Val = cast<ConstantSDNode>(RetValIdx)->getZExtValue();
  RetValIdx = CurDAG->getTargetConstant(Val, RetValIdx.getValueType());

  SDValue Ops[] = { N->getOperand(1), RetValIdx,
                    SDValue()/*The dummy bit width operand*/,
                    CurDAG->getTargetConstant(0, MVT::i64) /*and trace number*/,
                    N->getOperand(0) };

  computeOperandsBitWidth(N, Ops, array_lengthof(Ops) -1 /*Skip the chain*/);

  return CurDAG->SelectNodeTo(N, VTM::VOpRetVal, N->getVTList(),
                              Ops, array_lengthof(Ops));
}


SDNode *VDAGToDAGISel::SelectMemAccess(SDNode *N) {
  MachineSDNode::mmo_iterator MemOp = MF->allocateMemRefsArray(1);
  MemOp[0] = cast<MemSDNode>(N)->getMemOperand();

  SDValue Ops[] = { N->getOperand(1), N->getOperand(2), N->getOperand(3),
                    N->getOperand(4), SDValue()/*The dummy bit width operand*/,
                    CurDAG->getTargetConstant(0, MVT::i64) /*and trace number*/,
                    N->getOperand(0) };

  computeOperandsBitWidth(N, Ops, array_lengthof(Ops) -1 /*Skip the chain*/);
  unsigned Opc = VInstrInfo::isCmdSeq(N->getConstantOperandVal(3)) ?
                 VTM::VOpCmdSeq : VTM::VOpMemTrans;
  SDNode *Ret = CurDAG->SelectNodeTo(N, Opc, N->getVTList(),
                                     Ops, array_lengthof(Ops));

  cast<MachineSDNode>(Ret)->setMemRefs(MemOp, MemOp + 1);
  return Ret;
}

SDNode *VDAGToDAGISel::SelectBRamAccess(SDNode *N) {
  MachineSDNode::mmo_iterator MemOp = MF->allocateMemRefsArray(1);
  MemOp[0] = cast<MemIntrinsicSDNode>(N)->getMemOperand();

  unsigned ArgIdx = 2;
  unsigned BRamNum = N->getConstantOperandVal(ArgIdx + 5);

  SDValue Ops[] = { N->getOperand(ArgIdx), N->getOperand(ArgIdx + 1),
                    N->getOperand(ArgIdx + 2),
                    // FIXME: Set the correct byte enable.
                    CurDAG->getTargetConstant(0, MVT::i32),
                    CurDAG->getTargetConstant(BRamNum, MVT::i32),
                    SDValue()/*The dummy bit width operand*/,
                    CurDAG->getTargetConstant(0, MVT::i64) /*and trace number*/,
                    N->getOperand(0) };

  computeOperandsBitWidth(N, Ops, array_lengthof(Ops) -1 /*Skip the chain*/);

  SDNode *Ret = CurDAG->SelectNodeTo(N, VTM::VOpBRam, N->getVTList(),
                                     Ops, array_lengthof(Ops));

  cast<MachineSDNode>(Ret)->setMemRefs(MemOp, MemOp + 1);
  return Ret;
}

SDNode *VDAGToDAGISel::SelectINTRINSIC_W_CHAIN(SDNode *N) {
  unsigned IntNo = N->getConstantOperandVal(1);

  switch (IntNo) {
  default: break;
  case vtmIntrinsic::vtm_access_bram:
    return SelectBRamAccess(N);
  }

  return 0;
}

template<enum VFUs::FUTypes T, unsigned ChainedOpc, unsigned ControlOpc>
static unsigned SelectOpCode(unsigned FUSize) {
  return getFUDesc(T)->shouldBeChained(FUSize) ? ChainedOpc : ControlOpc;
}

template<enum VFUs::FUTypes T, unsigned ChainedOpc, unsigned ControlOpc>
static unsigned SelectOpCode(SDNode *N) {
  unsigned FUSize = VTargetLowering::computeSizeInBits(N->getOperand(0));
  return SelectOpCode<T, ChainedOpc, ControlOpc>(FUSize);
}

SDNode *VDAGToDAGISel::Select(SDNode *N) {
  if (N->isMachineOpcode())
    return 0;   // Already selected.

  switch (N->getOpcode()) {
  default: break;
  case VTMISD::ReadReturn:    return SelectSimpleNode(N, VTM::VOpReadReturn);
  case VTMISD::InternalCall:  return SelectInternalCall(N);
  case VTMISD::LoadArgument:  return SelectLoadArgument(N);
  case VTMISD::RetVal:        return SelectRetVal(N);
  case ISD::BR:
  case ISD::BRCOND:           return SelectBrcnd(N);

  case VTMISD::ADDCS:{
    unsigned FUSize = VTargetLowering::computeSizeInBits(SDValue(N, 0)) - 1;
    unsigned Opcode = SelectOpCode<VFUs::AddSub, VTM::VOpAdd_c, VTM::VOpAdd>(FUSize);
    return SelectSimpleNode(N, Opcode);
  }

  case VTMISD::ICmp:{
    unsigned Opcode = SelectOpCode<VFUs::ICmp, VTM::VOpICmp_c, VTM::VOpICmp>(N);
    return SelectSimpleNode(N, Opcode);
  }

  case ISD::MUL:{
    unsigned Opcode = SelectOpCode<VFUs::Mult, VTM::VOpMult_c, VTM::VOpMult>(N);
    return SelectBinary(N, Opcode);
  }

  case VTMISD::MULHiLo:{
    unsigned Opcode =
      SelectOpCode<VFUs::Mult, VTM::VOpMultLoHi_c, VTM::VOpMultLoHi>(N);
    return SelectBinary(N, Opcode);
  }

  case ISD::XOR:              return SelectBinary(N, VTM::VOpXor);
  case ISD::AND:              return SelectBinary(N, VTM::VOpAnd);
  case ISD::OR:               return SelectBinary(N, VTM::VOpOr);
  case VTMISD::Not:           return SelectUnary(N, VTM::VOpNot);
  case ISD::SELECT:           return SelectSimpleNode(N, VTM::VOpSel);

  case ISD::SHL:{
    unsigned Opcode = SelectOpCode<VFUs::Shift, VTM::VOpSHL_c, VTM::VOpSHL>(N);
    return SelectBinary(N, Opcode);
  }

  case ISD::SRL:{
    unsigned Opcode = SelectOpCode<VFUs::Shift, VTM::VOpSRL_c, VTM::VOpSRL>(N);
    return SelectBinary(N, Opcode);
  }

  case ISD::SRA:{
    unsigned Opcode = SelectOpCode<VFUs::Shift, VTM::VOpSRA_c, VTM::VOpSRA>(N);
    return SelectBinary(N, Opcode);
  }

  case VTMISD::BitRepeat:     return SelectBinary(N, VTM::VOpBitRepeat);
  case VTMISD::BitCat:        return SelectBinary(N, VTM::VOpBitCat);
  case VTMISD::BitSlice:      return SelectBitSlice(N);

  case VTMISD::ROr:           return SelectUnary(N, VTM::VOpROr);
  case VTMISD::RAnd:          return SelectUnary(N, VTM::VOpRAnd);
  case VTMISD::RXor:          return SelectUnary(N, VTM::VOpRXor);

  case ISD::GlobalAddress:
  case ISD::ExternalSymbol:
  case ISD::Constant:         return SelectImmediate(N);

  case VTMISD::MemAccess:     return SelectMemAccess(N);
  case ISD::INTRINSIC_W_CHAIN: return SelectINTRINSIC_W_CHAIN(N);
  }

  return SelectCode(N);
}
static void UpdateNodeOperand(SelectionDAG &DAG,  SDNode *N, unsigned Num,
                              SDValue Val) {
  SmallVector<SDValue, 8> ops(N->op_begin(), N->op_end());
  ops[Num] = Val;
  SDNode *New = DAG.UpdateNodeOperands(N, ops.data(), ops.size());
  DAG.ReplaceAllUsesWith(N, New);
}

void VDAGToDAGISel::CopyToReg(SelectionDAG &DAG, SDNode *Copy) {
  SDNode *SrcNode = Copy->getOperand(2).getNode();
  if (!shouldMoveToReg(SrcNode)) return;

  SDNode *MvImm = SelectImmediate(SrcNode, true);
  UpdateNodeOperand(DAG, Copy, 2, SDValue(MvImm, 0));
}

void VDAGToDAGISel::PostprocessISelDAG() {
  CurDAG->AssignTopologicalOrder();
  HandleSDNode Dummy(CurDAG->getRoot());

  for (SelectionDAG::allnodes_iterator NI = CurDAG->allnodes_begin();
       NI != CurDAG->allnodes_end(); ++NI) {
    if (NI->getOpcode() == ISD::CopyToReg)
      CopyToReg(*CurDAG, NI);
  }
  CurDAG->setRoot(Dummy.getValue());
}

void VDAGToDAGISel::LowerMemAccessISel(SDNode *N, SelectionDAG &DAG,
                                       bool isStore) {
  LSBaseSDNode *LSNode = cast<LSBaseSDNode>(N);
  // FIXME: Handle the index.
  assert(LSNode->isUnindexed() && "Indexed load/store is not supported!");

  EVT VT = LSNode->getMemoryVT();

  unsigned VTSize = VT.getSizeInBits();

  SDValue StoreVal = isStore ? cast<StoreSDNode>(LSNode)->getValue()
                             : DAG.getUNDEF(VT);

  LLVMContext *Cntx = DAG.getContext();
  EVT CmdVT = EVT::getIntegerVT(*Cntx, VFUMemBus::CMDWidth);
  SDValue SDOps[] = {// The chain.
                     LSNode->getChain(),
                     // The Value to store (if any), and the address.
                     LSNode->getBasePtr(), StoreVal,
                     // Is load?
                     DAG.getTargetConstant(isStore, CmdVT),
                     // Byte enable.
                     DAG.getTargetConstant(getByteEnable(VT.getStoreSize()),
                                           MVT::i8)
                    };

  unsigned DataBusWidth = getFUDesc<VFUMemBus>()->getDataWidth();
  assert(DataBusWidth >= VTSize && "Unexpected large data!");

  MVT DataBusVT =
    EVT::getIntegerVT(*DAG.getContext(), DataBusWidth).getSimpleVT();

  DebugLoc dl = N->getDebugLoc();
  SDValue Result  =
    DAG.getMemIntrinsicNode(VTMISD::MemAccess, dl,
                            // Result and the chain.
                            DAG.getVTList(DataBusVT, MVT::Other),
                            // SDValue operands
                            SDOps, array_lengthof(SDOps),
                            // Memory operands.
                            LSNode->getMemoryVT(), LSNode->getMemOperand());

  if (isStore)  {
    DAG.ReplaceAllUsesOfValueWith(SDValue(N, 0), Result.getValue(1));
    return;
  }

  SDValue Val = Result;
  // Truncate the data bus, the system bus should place the valid data start
  // from LSM.
  if (DataBusWidth > VTSize)
    Val = VTargetLowering::getTruncate(DAG, dl, Result, VTSize);

  // Check if this an extend load.
  LoadSDNode *LD = cast<LoadSDNode>(LSNode);
  ISD::LoadExtType ExtType = LD->getExtensionType();
  if (ExtType != ISD::NON_EXTLOAD) {
    unsigned DstSize = LD->getValueSizeInBits(0);
    Val = VTargetLowering::getExtend(DAG, dl, Val, DstSize,
                                     ExtType == ISD::SEXTLOAD);
  }

  // Do we need to replace the result of the load operation?
  SDValue NewValues[] = { Val, Result.getValue(1) };
  DAG.ReplaceAllUsesWith(N, NewValues);
}

void VDAGToDAGISel::LowerADDEForISel(SDNode *N, SelectionDAG &DAG) {
  SDValue LHS = N->getOperand(0), RHS = N->getOperand(1), C = N->getOperand(2);
  unsigned OpSize = VTargetLowering::computeSizeInBits(LHS);
  unsigned AddSize = OpSize + 1;
  DebugLoc dl = N->getDebugLoc();
  LLVMContext &Cntx = *DAG.getContext();

  EVT VT = LHS.getValueType(),
           NewVT = VTargetLowering::getRoundIntegerOrBitType(AddSize, Cntx);
  SDValue NewAdd = DAG.getNode(VTMISD::ADDCS, dl, NewVT, LHS, RHS, C);
  SDValue NewValues[] = {
    VTargetLowering::getBitSlice(DAG, dl, NewAdd, OpSize, 0),
    VTargetLowering::getBitSlice(DAG, dl, NewAdd, OpSize + 1, OpSize)
  };

  DAG.ReplaceAllUsesWith(N, NewValues);
}

void VDAGToDAGISel::LowerUMUL_LOHIForISel(SDNode *N, SelectionDAG &DAG){
  SDValue LHS = N->getOperand(0), RHS = N->getOperand(1);
  unsigned OpSize = VTargetLowering::computeSizeInBits(LHS);
  unsigned MulSize = OpSize * 2;
  assert(MulSize <= 64 && "Unsupported multiplier width!");

  DebugLoc dl = N->getDebugLoc();
  LLVMContext &Cntx = *DAG.getContext();

  EVT VT = LHS.getValueType(),
           NewVT = VTargetLowering::getRoundIntegerOrBitType(MulSize, Cntx);

  SDValue NewMul = DAG.getNode(VTMISD::MULHiLo, dl, NewVT, LHS, RHS);
  SDValue NewValues[] = {
    VTargetLowering::getBitSlice(DAG, dl, NewMul, OpSize, 0),
    VTargetLowering::getBitSlice(DAG, dl, NewMul, MulSize, OpSize)
  };

  DAG.ReplaceAllUsesWith(N, NewValues);
}

static unsigned getICmpPort(unsigned CC) {
  switch (CC) {
  case ISD::SETNE: return 1;
  case ISD::SETEQ: return 2;
  case ISD::SETGE: case ISD::SETUGE: return 3;
  case ISD::SETGT: case ISD::SETUGT: return 4;
  default: llvm_unreachable("Unexpected condition code!");
  }
}

void VDAGToDAGISel::LowerICmpForISel(SDNode *N, SelectionDAG &DAG) {
  CondCodeSDNode *CCNode = cast<CondCodeSDNode>(N->getOperand(2));

  DebugLoc dl = N->getDebugLoc();
  LLVMContext &Cntx = *DAG.getContext();

  SDValue LHS = N->getOperand(0), RHS = N->getOperand(1);
  unsigned OpSize = VTargetLowering::computeSizeInBits(LHS);
  //assert(OpSize > 1 && "Unexpected 1bit comparison!");
  EVT FUVT = EVT::getIntegerVT(Cntx, OpSize);
  ISD::CondCode CC = CCNode->get();

  switch (CC) {
  case ISD::SETEQ:
  case ISD::SETNE:
  case ISD::SETGT:
  case ISD::SETGE:
  case ISD::SETUGT:
  case ISD::SETUGE:
    break;
  case ISD::SETLT:
  case ISD::SETLE:
  case ISD::SETULT:
  case ISD::SETULE:
    CC = ISD::getSetCCSwappedOperands(CC);
    std::swap(LHS, RHS);
    break;
  default: llvm_unreachable("Unexpected CondCode!");
  }

  unsigned CCNum = (CC == ISD::SETEQ || CC == ISD::SETNE) ? VFUs::CmpEQ
    : (ISD::isSignedIntSetCC(CC) ? VFUs::CmpSigned
    : VFUs::CmpUnsigned);

  SDValue NewICmp = DAG.getNode(VTMISD::ICmp, dl, MVT::i8, LHS, RHS,
                                DAG.getTargetConstant(CCNum, FUVT));
  // Read the result from specific bit of the result.
  unsigned ResultPort = getICmpPort(CC);

  DAG.ReplaceAllUsesWith(SDValue(N, 0),
                         VTargetLowering::getBitSlice(DAG, dl, NewICmp,
                                                      ResultPort + 1, ResultPort,
                                                      N->getValueSizeInBits(0)));
}

void VDAGToDAGISel::PreprocessISelDAG() {
  // Create a dummy node (which is not added to allnodes), that adds a reference
  // to the root node, preventing it from being deleted, and tracking any
  // changes of the root.
  HandleSDNode Dummy(CurDAG->getRoot());

  // The root of the dag may dangle to deleted nodes until the dag combiner is
  // done.  Set it to null to avoid confusion.
  CurDAG->setRoot(SDValue());

  for (SelectionDAG::allnodes_iterator I = llvm::prior(CurDAG->allnodes_end()),
       E = CurDAG->allnodes_begin(); I != E; /*--I*/) {
    SDNode *N = I;

    switch (N->getOpcode()) {
    default: --I; continue;
    case ISD::ADDE:
      LowerADDEForISel(N, *CurDAG);
      break;
    case VTMISD::ICmp:
      LowerICmpForISel(N, *CurDAG);
      break;
    case ISD::UMUL_LOHI:
      LowerUMUL_LOHIForISel(N, *CurDAG);
      break;
    case ISD::LOAD:
    case ISD::STORE:
      LowerMemAccessISel(N, *CurDAG, N->getOpcode() == ISD::STORE);
      break;
    }

    --I;
    CurDAG->DeleteNode(N);
  }

  // If the root changed (e.g. it was a dead load, update the root).
  CurDAG->setRoot(Dummy.getValue());

  // Combine the bitslices produced by lower for ISel.
  CurDAG->Combine(NoIllegalOperations, *AA, OptLevel);
}
