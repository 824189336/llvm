//===--------------------- InstrBuilder.cpp ---------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
/// \file
///
/// This file implements the InstrBuilder interface.
///
//===----------------------------------------------------------------------===//

#include "InstrBuilder.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/MC/MCInst.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "llvm-mca"

namespace mca {

using namespace llvm;

static void initializeUsedResources(InstrDesc &ID,
                                    const MCSchedClassDesc &SCDesc,
                                    const MCSubtargetInfo &STI,
                                    ArrayRef<uint64_t> ProcResourceMasks) {
  const MCSchedModel &SM = STI.getSchedModel();

  // Populate resources consumed.
  using ResourcePlusCycles = std::pair<uint64_t, ResourceUsage>;
  std::vector<ResourcePlusCycles> Worklist;

  // Track cycles contributed by resources that are in a "Super" relationship.
  // This is required if we want to correctly match the behavior of method
  // SubtargetEmitter::ExpandProcResource() in Tablegen. When computing the set
  // of "consumed" processor resources and resource cycles, the logic in
  // ExpandProcResource() doesn't update the number of resource cycles
  // contributed by a "Super" resource to a group.
  // We need to take this into account when we find that a processor resource is
  // part of a group, and it is also used as the "Super" of other resources.
  // This map stores the number of cycles contributed by sub-resources that are
  // part of a "Super" resource. The key value is the "Super" resource mask ID.
  DenseMap<uint64_t, unsigned> SuperResources;

  for (unsigned I = 0, E = SCDesc.NumWriteProcResEntries; I < E; ++I) {
    const MCWriteProcResEntry *PRE = STI.getWriteProcResBegin(&SCDesc) + I;
    const MCProcResourceDesc &PR = *SM.getProcResource(PRE->ProcResourceIdx);
    uint64_t Mask = ProcResourceMasks[PRE->ProcResourceIdx];
    if (PR.BufferSize != -1)
      ID.Buffers.push_back(Mask);
    CycleSegment RCy(0, PRE->Cycles, false);
    Worklist.emplace_back(ResourcePlusCycles(Mask, ResourceUsage(RCy)));
    if (PR.SuperIdx) {
      uint64_t Super = ProcResourceMasks[PR.SuperIdx];
      SuperResources[Super] += PRE->Cycles;
    }
  }

  // Sort elements by mask popcount, so that we prioritize resource units over
  // resource groups, and smaller groups over larger groups.
  sort(Worklist, [](const ResourcePlusCycles &A, const ResourcePlusCycles &B) {
    unsigned popcntA = countPopulation(A.first);
    unsigned popcntB = countPopulation(B.first);
    if (popcntA < popcntB)
      return true;
    if (popcntA > popcntB)
      return false;
    return A.first < B.first;
  });

  uint64_t UsedResourceUnits = 0;

  // Remove cycles contributed by smaller resources.
  for (unsigned I = 0, E = Worklist.size(); I < E; ++I) {
    ResourcePlusCycles &A = Worklist[I];
    if (!A.second.size()) {
      A.second.NumUnits = 0;
      A.second.setReserved();
      ID.Resources.emplace_back(A);
      continue;
    }

    ID.Resources.emplace_back(A);
    uint64_t NormalizedMask = A.first;
    if (countPopulation(A.first) == 1) {
      UsedResourceUnits |= A.first;
    } else {
      // Remove the leading 1 from the resource group mask.
      NormalizedMask ^= PowerOf2Floor(NormalizedMask);
    }

    for (unsigned J = I + 1; J < E; ++J) {
      ResourcePlusCycles &B = Worklist[J];
      if ((NormalizedMask & B.first) == NormalizedMask) {
        B.second.CS.subtract(A.second.size() - SuperResources[A.first]);
        if (countPopulation(B.first) > 1)
          B.second.NumUnits++;
      }
    }
  }

  // A SchedWrite may specify a number of cycles in which a resource group
  // is reserved. For example (on target x86; cpu Haswell):
  //
  //  SchedWriteRes<[HWPort0, HWPort1, HWPort01]> {
  //    let ResourceCycles = [2, 2, 3];
  //  }
  //
  // This means:
  // Resource units HWPort0 and HWPort1 are both used for 2cy.
  // Resource group HWPort01 is the union of HWPort0 and HWPort1.
  // Since this write touches both HWPort0 and HWPort1 for 2cy, HWPort01
  // will not be usable for 2 entire cycles from instruction issue.
  //
  // On top of those 2cy, SchedWriteRes explicitly specifies an extra latency
  // of 3 cycles for HWPort01. This tool assumes that the 3cy latency is an
  // extra delay on top of the 2 cycles latency.
  // During those extra cycles, HWPort01 is not usable by other instructions.
  for (ResourcePlusCycles &RPC : ID.Resources) {
    if (countPopulation(RPC.first) > 1 && !RPC.second.isReserved()) {
      // Remove the leading 1 from the resource group mask.
      uint64_t Mask = RPC.first ^ PowerOf2Floor(RPC.first);
      if ((Mask & UsedResourceUnits) == Mask)
        RPC.second.setReserved();
    }
  }

  LLVM_DEBUG({
    for (const std::pair<uint64_t, ResourceUsage> &R : ID.Resources)
      dbgs() << "\t\tMask=" << R.first << ", cy=" << R.second.size() << '\n';
    for (const uint64_t R : ID.Buffers)
      dbgs() << "\t\tBuffer Mask=" << R << '\n';
  });
}

static void computeMaxLatency(InstrDesc &ID, const MCInstrDesc &MCDesc,
                              const MCSchedClassDesc &SCDesc,
                              const MCSubtargetInfo &STI) {
  if (MCDesc.isCall()) {
    // We cannot estimate how long this call will take.
    // Artificially set an arbitrarily high latency (100cy).
    ID.MaxLatency = 100U;
    return;
  }

  int Latency = MCSchedModel::computeInstrLatency(STI, SCDesc);
  // If latency is unknown, then conservatively assume a MaxLatency of 100cy.
  ID.MaxLatency = Latency < 0 ? 100U : static_cast<unsigned>(Latency);
}

Error InstrBuilder::populateWrites(InstrDesc &ID, const MCInst &MCI,
                                   unsigned SchedClassID) {
  const MCInstrDesc &MCDesc = MCII.get(MCI.getOpcode());
  const MCSchedModel &SM = STI.getSchedModel();
  const MCSchedClassDesc &SCDesc = *SM.getSchedClassDesc(SchedClassID);

  // These are for now the (strong) assumptions made by this algorithm:
  //  * The number of explicit and implicit register definitions in a MCInst
  //    matches the number of explicit and implicit definitions according to
  //    the opcode descriptor (MCInstrDesc).
  //  * Register definitions take precedence over register uses in the operands
  //    list.
  //  * If an opcode specifies an optional definition, then the optional
  //    definition is always the last operand in the sequence, and it can be
  //    set to zero (i.e. "no register").
  //
  // These assumptions work quite well for most out-of-order in-tree targets
  // like x86. This is mainly because the vast majority of instructions is
  // expanded to MCInst using a straightforward lowering logic that preserves
  // the ordering of the operands.
  unsigned NumExplicitDefs = MCDesc.getNumDefs();
  unsigned NumImplicitDefs = MCDesc.getNumImplicitDefs();
  unsigned NumWriteLatencyEntries = SCDesc.NumWriteLatencyEntries;
  unsigned TotalDefs = NumExplicitDefs + NumImplicitDefs;
  if (MCDesc.hasOptionalDef())
    TotalDefs++;
  ID.Writes.resize(TotalDefs);
  // Iterate over the operands list, and skip non-register operands.
  // The first NumExplictDefs register operands are expected to be register
  // definitions.
  unsigned CurrentDef = 0;
  unsigned i = 0;
  for (; i < MCI.getNumOperands() && CurrentDef < NumExplicitDefs; ++i) {
    const MCOperand &Op = MCI.getOperand(i);
    if (!Op.isReg())
      continue;

    WriteDescriptor &Write = ID.Writes[CurrentDef];
    Write.OpIndex = i;
    if (CurrentDef < NumWriteLatencyEntries) {
      const MCWriteLatencyEntry &WLE =
          *STI.getWriteLatencyEntry(&SCDesc, CurrentDef);
      // Conservatively default to MaxLatency.
      Write.Latency =
          WLE.Cycles < 0 ? ID.MaxLatency : static_cast<unsigned>(WLE.Cycles);
      Write.SClassOrWriteResourceID = WLE.WriteResourceID;
    } else {
      // Assign a default latency for this write.
      Write.Latency = ID.MaxLatency;
      Write.SClassOrWriteResourceID = 0;
    }
    Write.IsOptionalDef = false;
    LLVM_DEBUG({
      dbgs() << "\t\t[Def] OpIdx=" << Write.OpIndex
             << ", Latency=" << Write.Latency
             << ", WriteResourceID=" << Write.SClassOrWriteResourceID << '\n';
    });
    CurrentDef++;
  }

  if (CurrentDef != NumExplicitDefs) {
    return make_error<InstructionError<MCInst>>(
        "Expected more register operand definitions.", MCI);
  }

  CurrentDef = 0;
  for (CurrentDef = 0; CurrentDef < NumImplicitDefs; ++CurrentDef) {
    unsigned Index = NumExplicitDefs + CurrentDef;
    WriteDescriptor &Write = ID.Writes[Index];
    Write.OpIndex = ~CurrentDef;
    Write.RegisterID = MCDesc.getImplicitDefs()[CurrentDef];
    if (Index < NumWriteLatencyEntries) {
      const MCWriteLatencyEntry &WLE =
          *STI.getWriteLatencyEntry(&SCDesc, Index);
      // Conservatively default to MaxLatency.
      Write.Latency =
          WLE.Cycles < 0 ? ID.MaxLatency : static_cast<unsigned>(WLE.Cycles);
      Write.SClassOrWriteResourceID = WLE.WriteResourceID;
    } else {
      // Assign a default latency for this write.
      Write.Latency = ID.MaxLatency;
      Write.SClassOrWriteResourceID = 0;
    }

    Write.IsOptionalDef = false;
    assert(Write.RegisterID != 0 && "Expected a valid phys register!");
    LLVM_DEBUG({
      dbgs() << "\t\t[Def] OpIdx=" << Write.OpIndex
             << ", PhysReg=" << MRI.getName(Write.RegisterID)
             << ", Latency=" << Write.Latency
             << ", WriteResourceID=" << Write.SClassOrWriteResourceID << '\n';
    });
  }

  if (MCDesc.hasOptionalDef()) {
    // Always assume that the optional definition is the last operand of the
    // MCInst sequence.
    const MCOperand &Op = MCI.getOperand(MCI.getNumOperands() - 1);
    if (i == MCI.getNumOperands() || !Op.isReg()) {
      std::string Message =
          "expected a register operand for an optional definition. Instruction "
          "has not been correctly analyzed.";
      return make_error<InstructionError<MCInst>>(Message, MCI);
    }

    WriteDescriptor &Write = ID.Writes[TotalDefs - 1];
    Write.OpIndex = MCI.getNumOperands() - 1;
    // Assign a default latency for this write.
    Write.Latency = ID.MaxLatency;
    Write.SClassOrWriteResourceID = 0;
    Write.IsOptionalDef = true;
  }

  return ErrorSuccess();
}

Error InstrBuilder::populateReads(InstrDesc &ID, const MCInst &MCI,
                                  unsigned SchedClassID) {
  const MCInstrDesc &MCDesc = MCII.get(MCI.getOpcode());
  unsigned NumExplicitDefs = MCDesc.getNumDefs();

  // Skip explicit definitions.
  unsigned i = 0;
  for (; i < MCI.getNumOperands() && NumExplicitDefs; ++i) {
    const MCOperand &Op = MCI.getOperand(i);
    if (Op.isReg())
      NumExplicitDefs--;
  }

  if (NumExplicitDefs) {
    return make_error<InstructionError<MCInst>>(
        "Expected more register operand definitions.", MCI);
  }

  unsigned NumExplicitUses = MCI.getNumOperands() - i;
  unsigned NumImplicitUses = MCDesc.getNumImplicitUses();
  if (MCDesc.hasOptionalDef()) {
    assert(NumExplicitUses);
    NumExplicitUses--;
  }
  unsigned TotalUses = NumExplicitUses + NumImplicitUses;
  if (!TotalUses)
    return ErrorSuccess();

  ID.Reads.resize(TotalUses);
  for (unsigned CurrentUse = 0; CurrentUse < NumExplicitUses; ++CurrentUse) {
    ReadDescriptor &Read = ID.Reads[CurrentUse];
    Read.OpIndex = i + CurrentUse;
    Read.UseIndex = CurrentUse;
    Read.SchedClassID = SchedClassID;
    LLVM_DEBUG(dbgs() << "\t\t[Use] OpIdx=" << Read.OpIndex
                      << ", UseIndex=" << Read.UseIndex << '\n');
  }

  for (unsigned CurrentUse = 0; CurrentUse < NumImplicitUses; ++CurrentUse) {
    ReadDescriptor &Read = ID.Reads[NumExplicitUses + CurrentUse];
    Read.OpIndex = ~CurrentUse;
    Read.UseIndex = NumExplicitUses + CurrentUse;
    Read.RegisterID = MCDesc.getImplicitUses()[CurrentUse];
    Read.SchedClassID = SchedClassID;
    LLVM_DEBUG(dbgs() << "\t\t[Use] OpIdx=" << Read.OpIndex << ", RegisterID="
                      << MRI.getName(Read.RegisterID) << '\n');
  }
  return ErrorSuccess();
}

Error InstrBuilder::verifyInstrDesc(const InstrDesc &ID,
                                    const MCInst &MCI) const {
  if (ID.NumMicroOps != 0)
    return ErrorSuccess();

  bool UsesMemory = ID.MayLoad || ID.MayStore;
  bool UsesBuffers = !ID.Buffers.empty();
  bool UsesResources = !ID.Resources.empty();
  if (!UsesMemory && !UsesBuffers && !UsesResources)
    return ErrorSuccess();

  StringRef Message;
  if (UsesMemory) {
    Message = "found an inconsistent instruction that decodes "
              "into zero opcodes and that consumes load/store "
              "unit resources.";
  } else {
    Message = "found an inconsistent instruction that decodes "
              "to zero opcodes and that consumes scheduler "
              "resources.";
  }

  return make_error<InstructionError<MCInst>>(Message, MCI);
}

Expected<const InstrDesc &>
InstrBuilder::createInstrDescImpl(const MCInst &MCI) {
  assert(STI.getSchedModel().hasInstrSchedModel() &&
         "Itineraries are not yet supported!");

  // Obtain the instruction descriptor from the opcode.
  unsigned short Opcode = MCI.getOpcode();
  const MCInstrDesc &MCDesc = MCII.get(Opcode);
  const MCSchedModel &SM = STI.getSchedModel();

  // Then obtain the scheduling class information from the instruction.
  unsigned SchedClassID = MCDesc.getSchedClass();
  unsigned CPUID = SM.getProcessorID();

  // Try to solve variant scheduling classes.
  if (SchedClassID) {
    while (SchedClassID && SM.getSchedClassDesc(SchedClassID)->isVariant())
      SchedClassID = STI.resolveVariantSchedClass(SchedClassID, &MCI, CPUID);

    if (!SchedClassID) {
      return make_error<InstructionError<MCInst>>(
          "unable to resolve scheduling class for write variant.", MCI);
    }
  }

  // Check if this instruction is supported. Otherwise, report an error.
  const MCSchedClassDesc &SCDesc = *SM.getSchedClassDesc(SchedClassID);
  if (SCDesc.NumMicroOps == MCSchedClassDesc::InvalidNumMicroOps) {
    return make_error<InstructionError<MCInst>>(
        "found an unsupported instruction in the input assembly sequence.",
        MCI);
  }

  // Create a new empty descriptor.
  std::unique_ptr<InstrDesc> ID = llvm::make_unique<InstrDesc>();
  ID->NumMicroOps = SCDesc.NumMicroOps;

  if (MCDesc.isCall()) {
    // We don't correctly model calls.
    WithColor::warning() << "found a call in the input assembly sequence.\n";
    WithColor::note() << "call instructions are not correctly modeled. "
                      << "Assume a latency of 100cy.\n";
  }

  if (MCDesc.isReturn()) {
    WithColor::warning() << "found a return instruction in the input"
                         << " assembly sequence.\n";
    WithColor::note() << "program counter updates are ignored.\n";
  }

  ID->MayLoad = MCDesc.mayLoad();
  ID->MayStore = MCDesc.mayStore();
  ID->HasSideEffects = MCDesc.hasUnmodeledSideEffects();

  initializeUsedResources(*ID, SCDesc, STI, ProcResourceMasks);
  computeMaxLatency(*ID, MCDesc, SCDesc, STI);
  if (auto Err = populateWrites(*ID, MCI, SchedClassID))
    return std::move(Err);
  if (auto Err = populateReads(*ID, MCI, SchedClassID))
    return std::move(Err);

  LLVM_DEBUG(dbgs() << "\t\tMaxLatency=" << ID->MaxLatency << '\n');
  LLVM_DEBUG(dbgs() << "\t\tNumMicroOps=" << ID->NumMicroOps << '\n');

  // Sanity check on the instruction descriptor.
  if (Error Err = verifyInstrDesc(*ID, MCI))
    return std::move(Err);

  // Now add the new descriptor.
  SchedClassID = MCDesc.getSchedClass();
  if (!SM.getSchedClassDesc(SchedClassID)->isVariant()) {
    Descriptors[MCI.getOpcode()] = std::move(ID);
    return *Descriptors[MCI.getOpcode()];
  }

  VariantDescriptors[&MCI] = std::move(ID);
  return *VariantDescriptors[&MCI];
}

Expected<const InstrDesc &>
InstrBuilder::getOrCreateInstrDesc(const MCInst &MCI) {
  if (Descriptors.find_as(MCI.getOpcode()) != Descriptors.end())
    return *Descriptors[MCI.getOpcode()];

  if (VariantDescriptors.find(&MCI) != VariantDescriptors.end())
    return *VariantDescriptors[&MCI];

  return createInstrDescImpl(MCI);
}

Expected<std::unique_ptr<Instruction>>
InstrBuilder::createInstruction(const MCInst &MCI) {
  Expected<const InstrDesc &> DescOrErr = getOrCreateInstrDesc(MCI);
  if (!DescOrErr)
    return DescOrErr.takeError();
  const InstrDesc &D = *DescOrErr;
  std::unique_ptr<Instruction> NewIS = llvm::make_unique<Instruction>(D);

  // Check if this is a dependency breaking instruction.
  APInt Mask;

  unsigned ProcID = STI.getSchedModel().getProcessorID();
  bool IsZeroIdiom = MCIA.isZeroIdiom(MCI, Mask, ProcID);
  bool IsDepBreaking =
      IsZeroIdiom || MCIA.isDependencyBreaking(MCI, Mask, ProcID);
  if (MCIA.isOptimizableRegisterMove(MCI, ProcID))
    NewIS->setOptimizableMove();

  // Initialize Reads first.
  for (const ReadDescriptor &RD : D.Reads) {
    int RegID = -1;
    if (!RD.isImplicitRead()) {
      // explicit read.
      const MCOperand &Op = MCI.getOperand(RD.OpIndex);
      // Skip non-register operands.
      if (!Op.isReg())
        continue;
      RegID = Op.getReg();
    } else {
      // Implicit read.
      RegID = RD.RegisterID;
    }

    // Skip invalid register operands.
    if (!RegID)
      continue;

    // Okay, this is a register operand. Create a ReadState for it.
    assert(RegID > 0 && "Invalid register ID found!");
    auto RS = llvm::make_unique<ReadState>(RD, RegID);

    if (IsDepBreaking) {
      // A mask of all zeroes means: explicit input operands are not
      // independent.
      if (Mask.isNullValue()) {
        if (!RD.isImplicitRead())
          RS->setIndependentFromDef();
      } else {
        // Check if this register operand is independent according to `Mask`.
        // Note that Mask may not have enough bits to describe all explicit and
        // implicit input operands. If this register operand doesn't have a
        // corresponding bit in Mask, then conservatively assume that it is
        // dependent.
        if (Mask.getBitWidth() > RD.UseIndex) {
          // Okay. This map describe register use `RD.UseIndex`.
          if (Mask[RD.UseIndex])
            RS->setIndependentFromDef();
        }
      }
    }
    NewIS->getUses().emplace_back(std::move(RS));
  }

  // Early exit if there are no writes.
  if (D.Writes.empty())
    return std::move(NewIS);

  // Track register writes that implicitly clear the upper portion of the
  // underlying super-registers using an APInt.
  APInt WriteMask(D.Writes.size(), 0);

  // Now query the MCInstrAnalysis object to obtain information about which
  // register writes implicitly clear the upper portion of a super-register.
  MCIA.clearsSuperRegisters(MRI, MCI, WriteMask);

  // Initialize writes.
  unsigned WriteIndex = 0;
  for (const WriteDescriptor &WD : D.Writes) {
    unsigned RegID = WD.isImplicitWrite() ? WD.RegisterID
                                          : MCI.getOperand(WD.OpIndex).getReg();
    // Check if this is a optional definition that references NoReg.
    if (WD.IsOptionalDef && !RegID) {
      ++WriteIndex;
      continue;
    }

    assert(RegID && "Expected a valid register ID!");
    NewIS->getDefs().emplace_back(llvm::make_unique<WriteState>(
        WD, RegID, /* ClearsSuperRegs */ WriteMask[WriteIndex],
        /* WritesZero */ IsZeroIdiom));
    ++WriteIndex;
  }

  return std::move(NewIS);
}
} // namespace mca
