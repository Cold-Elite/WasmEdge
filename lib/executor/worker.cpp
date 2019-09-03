#include "executor/worker.h"
#include "ast/common.h"
#include "ast/instruction.h"
#include "executor/worker/util.h"
#include "support/casting.h"

namespace SSVM {
namespace Executor {

namespace {
using OpCode = AST::Instruction::OpCode;
using Value = AST::ValVariant;
} // namespace

ErrCode Worker::setArguments(Bytes &Input) {
  Args.assign(Input.begin(), Input.end());
  return ErrCode::Success;
}

ErrCode Worker::runExpression(const InstrVec &Instrs) {
  /// Check worker's flow.
  if (TheState != State::Inited)
    return ErrCode::WrongWorkerFlow;

  /// Set instruction vector to instruction provider.
  InstrPdr.pushInstrs(InstrProvider::SeqType::Expression, Instrs);
  TheState = State::CodeSet;
  return runLoop();
}

ErrCode Worker::runStartFunction(unsigned int FuncAddr) {
  /// Check worker's flow.
  if (TheState != State::Inited)
    return ErrCode::WrongWorkerFlow;

  /// TODO: Push arguments of start function into stack.

  /// Enter start function.
  ErrCode Status = ErrCode::Success;
  if ((Status = invokeFunction(FuncAddr)) != ErrCode::Success)
    return Status;

  /// Execute run loop.
  TheState = State::CodeSet;
  if ((Status = runLoop()) != ErrCode::Success)
    return Status;

  /// TODO: Pop return value.
  return Status;
}

ErrCode Worker::runLoop() {
  /// Check worker's flow
  if (TheState == State::Unreachable)
    return ErrCode::Unreachable;
  if (TheState != State::CodeSet)
    return ErrCode::WrongWorkerFlow;

  /// Run instructions
  ErrCode Status = ErrCode::Success;
  AST::Instruction *Instr = nullptr;
  TheState = State::Active;
  while (InstrPdr.getScopeSize() > 0 && Status == ErrCode::Success) {
    Instr = InstrPdr.getNextInstr();
    if (Instr == nullptr) {
      /// Pop instruction sequence.
      if (InstrPdr.getTopScopeType() == InstrProvider::SeqType::FunctionCall)
        Status = returnFunction();
      else if (InstrPdr.getTopScopeType() == InstrProvider::SeqType::Block)
        Status = leaveBlock();
      else
        Status = InstrPdr.popInstrs();
    } else {
      /// Run instructions.
      OpCode Opcode = Instr->getOpCode();
      if (isConstNumericOp(Opcode)) {
        Status = runConstNumericOp(Instr);
      } else if (isControlOp(Opcode)) {
        Status = runControlOp(Instr);
      } else if (isNumericOp(Opcode)) {
        Status = runNumericOp(Instr);
      } else if (isMemoryOp(Opcode)) {
        Status = runMemoryOp(Instr);
      } else if (isParametricOp(Opcode)) {
        Status = runParametricOp(Instr);
      } else if (isVariableOp(Opcode)) {
        Status = runVariableOp(Instr);
      }
    }
  }

  /// Check result
  if (TheState == State::Unreachable)
    return ErrCode::Unreachable;
  TheState = State::Inited;
  return Status;
}

ErrCode Worker::runConstNumericOp(AST::Instruction *InstrPtr) {
  auto TheInstrPtr = dynamic_cast<AST::ConstInstruction *>(InstrPtr);
  if (TheInstrPtr == nullptr) {
    return ErrCode::InstructionTypeMismatch;
  }

  std::unique_ptr<ValueEntry> VE = nullptr;
  std::visit([&VE](auto &&arg) { VE = std::make_unique<ValueEntry>(arg); },
             TheInstrPtr->value());

  StackMgr.push(VE);

  return ErrCode::Success;
}

ErrCode Worker::runNumericOp(AST::Instruction *InstrPtr) {
  auto TheInstrPtr = dynamic_cast<AST::NumericInstruction *>(InstrPtr);
  if (TheInstrPtr == nullptr) {
    return ErrCode::InstructionTypeMismatch;
  }

  auto Opcode = TheInstrPtr->getOpCode();
  auto Status = ErrCode::Success;
  if (isBinaryOp(Opcode)) {
    std::unique_ptr<ValueEntry> Val1, Val2;
    StackMgr.pop(Val2);
    StackMgr.pop(Val1);

    if (isValueTypeEqual(*Val1.get(), *Val2.get())) {
      return ErrCode::TypeNotMatch;
    }

    switch (Opcode) {
    case OpCode::I32__add:
      Status = runAddOp<int32_t>(Val1.get(), Val2.get());
      break;
    case OpCode::I32__sub:
      Status = runSubOp<int32_t>(Val1.get(), Val2.get());
      break;
    case OpCode::I64__add:
      Status = runAddOp<int64_t>(Val1.get(), Val2.get());
      break;
    case OpCode::I64__sub:
      Status = runSubOp<int64_t>(Val1.get(), Val2.get());
      break;
    case OpCode::I64__mul:
      Status = runMulOp<int64_t>(Val1.get(), Val2.get());
      break;
    case OpCode::I64__div_u:
      Status = runDivUOp<int64_t>(Val1.get(), Val2.get());
      break;
    case OpCode::I64__rem_u:
      Status = runModUOp<int64_t>(Val1.get(), Val2.get());
      break;
    default:
      Status = ErrCode::Unimplemented;
      break;
    }
  } else if (isComparisonOp(Opcode)) {
    std::unique_ptr<ValueEntry> Val1, Val2;
    StackMgr.pop(Val2);
    StackMgr.pop(Val1);

    if (isValueTypeEqual(*Val1.get(), *Val2.get())) {
      return ErrCode::TypeNotMatch;
    }

    switch (Opcode) {
    case OpCode::I32__le_s:
      Status = runLeSOp<int32_t>(Val1.get(), Val2.get());
      break;
    case OpCode::I32__eq:
      Status = runEqOp<int32_t>(Val1.get(), Val2.get());
      break;
    case OpCode::I32__ne:
      Status = runNeOp<int32_t>(Val1.get(), Val2.get());
      break;
    case OpCode::I64__eq:
      Status = runEqOp<int64_t>(Val1.get(), Val2.get());
      break;
    case OpCode::I64__lt_u:
      Status = runLtUOp<int64_t>(Val1.get(), Val2.get());
      break;
    default:
      Status = ErrCode::Unimplemented;
      break;
    }
  } else {
    Status = ErrCode::Unimplemented;
  }
  return Status;
}

ErrCode Worker::runControlOp(AST::Instruction *InstrPtr) {
  auto TheInstrPtr = dynamic_cast<AST::ControlInstruction *>(InstrPtr);
  if (TheInstrPtr == nullptr) {
    return ErrCode::InstructionTypeMismatch;
  }

  auto Status = ErrCode::Success;
  switch (TheInstrPtr->getOpCode()) {
  case OpCode::Unreachable:
    TheState = State::Unreachable;
    Status = ErrCode::Unreachable;
    break;
  case OpCode::Block:
    Status = runBlockOp(TheInstrPtr);
    break;
  case OpCode::Br:
    Status = runBrOp(TheInstrPtr);
    break;
  case OpCode::Br_if:
    Status = runBrIfOp(TheInstrPtr);
    break;
  case OpCode::Return:
    Status = runReturnOp();
    break;
  case OpCode::Call:
    Status = runCallOp(TheInstrPtr);
    break;
  default:
    Status = ErrCode::Unimplemented;
    break;
  }

  return ErrCode::Success;
}

ErrCode Worker::runMemoryOp(AST::Instruction *InstrPtr) {
  auto TheInstrPtr = dynamic_cast<AST::MemoryInstruction *>(InstrPtr);
  if (TheInstrPtr == nullptr) {
    return ErrCode::InstructionTypeMismatch;
  }

  auto Status = ErrCode::Success;
  auto Opcode = TheInstrPtr->getOpCode();
  if (isLoadOp(Opcode)) {
    switch (Opcode) {
    case OpCode::I32__load:
      Status = runLoadOp<int32_t>(TheInstrPtr);
      break;
    case OpCode::I64__load:
      Status = runLoadOp<int64_t>(TheInstrPtr);
      break;
    default:
      Status = ErrCode::Unimplemented;
      break;
    }
  } else if (isStoreOp(Opcode)) {
    switch (Opcode) {
    case OpCode::I32__store:
      Status = runStoreOp<int32_t>(TheInstrPtr);
      break;
    case OpCode::I64__store:
      Status = runStoreOp<int64_t>(TheInstrPtr);
      break;
    default:
      Status = ErrCode::Unimplemented;
      break;
    }
  } else {
    Status = ErrCode::Unimplemented;
  }

  return Status;
}

ErrCode Worker::runParametricOp(AST::Instruction *InstrPtr) {
  auto TheInstrPtr = dynamic_cast<AST::ParametricInstruction *>(InstrPtr);
  if (TheInstrPtr == nullptr) {
    return ErrCode::InstructionTypeMismatch;
  }

  if (TheInstrPtr->getOpCode() == OpCode::Drop) {
    StackMgr.pop();
  } else if (TheInstrPtr->getOpCode() == OpCode::Select) {

    // Pop the value i32.const from the stack.
    std::unique_ptr<ValueEntry> VE;
    StackMgr.pop(VE);
    int32_t Val;
    VE->getValue(Val);

    std::unique_ptr<ValueEntry> Val1, Val2;
    StackMgr.pop(Val2);
    StackMgr.pop(Val1);

    if (Val == 0) {
      StackMgr.push(Val2);
    } else {
      StackMgr.push(Val1);
    }
  } else {
    return ErrCode::InstructionTypeMismatch;
  }
  return ErrCode::Success;
}

ErrCode Worker::runVariableOp(AST::Instruction *InstrPtr) {
  auto TheInstrPtr = dynamic_cast<AST::VariableInstruction *>(InstrPtr);
  if (TheInstrPtr == nullptr) {
    return ErrCode::InstructionTypeMismatch;
  }

  auto Opcode = TheInstrPtr->getOpCode();
  unsigned int Index = TheInstrPtr->getIndex();

  if (Opcode == OpCode::Local__get) {
    StackMgr.getCurrentFrame(CurrentFrame);
    ValueEntry *Val;
    CurrentFrame->getValue(Index, Val);
    std::unique_ptr<ValueEntry> NewVal = std::make_unique<ValueEntry>(*Val);
    StackMgr.push(NewVal);
  } else if (Opcode == OpCode::Local__set) {
    StackMgr.getCurrentFrame(CurrentFrame);
    std::unique_ptr<ValueEntry> Val;
    StackMgr.pop(Val);
    CurrentFrame->setValue(Index, Val);
  } else if (Opcode == OpCode::Local__tee) {
    std::unique_ptr<ValueEntry> Val;
    StackMgr.pop(Val);
    std::unique_ptr<ValueEntry> NewVal =
        std::make_unique<ValueEntry>(*Val.get());
    StackMgr.push(NewVal);
    CurrentFrame->setValue(Index, Val);
  } else if (Opcode == OpCode::Global__get) {
    StackMgr.getCurrentFrame(CurrentFrame);
    ValueEntry Val;
    unsigned int ModuleAddr = CurrentFrame->getModuleAddr();
    Instance::ModuleInstance *ModuleInstPtr = nullptr;
    StoreMgr.getModule(ModuleAddr, ModuleInstPtr);
    unsigned int GlobalAddr;
    ModuleInstPtr->getGlobalAddr(Index, GlobalAddr);
    Instance::GlobalInstance *GlobPtr = nullptr;
    StoreMgr.getGlobal(GlobalAddr, GlobPtr);
    GlobPtr->getValue(Val);
    std::unique_ptr<ValueEntry> NewVal = std::make_unique<ValueEntry>(Val);
    StackMgr.push(NewVal);
  } else if (Opcode == OpCode::Global__set) {
    StackMgr.getCurrentFrame(CurrentFrame);
    unsigned int ModuleAddr = CurrentFrame->getModuleAddr();
    Instance::ModuleInstance *ModuleInstPtr = nullptr;
    StoreMgr.getModule(ModuleAddr, ModuleInstPtr);
    unsigned int GlobalAddr;
    ModuleInstPtr->getGlobalAddr(Index, GlobalAddr);
    Instance::GlobalInstance *GlobPtr = nullptr;
    StoreMgr.getGlobal(GlobalAddr, GlobPtr);
    std::unique_ptr<ValueEntry> Val;
    StackMgr.pop(Val);
    GlobPtr->setValue(*Val.get());
  } else {
    return ErrCode::InstructionTypeMismatch;
  }

  return ErrCode::Success;
}

ErrCode Worker::enterBlock(unsigned int Arity, AST::Instruction *Instr,
                           const InstrVec &Seq) {
  /// Create label for block.
  std::unique_ptr<LabelEntry> Label;
  if (Instr == nullptr)
    Label = std::make_unique<LabelEntry>(Arity);
  else
    Label = std::make_unique<LabelEntry>(Arity, Instr);

  /// Push label and jump to block body.
  StackMgr.push(Label);
  return InstrPdr.pushInstrs(InstrProvider::SeqType::Block, Seq);
}

ErrCode Worker::leaveBlock() {
  /// Pop top values on stack until a label.
  std::vector<std::unique_ptr<ValueEntry>> Vals;
  while (!StackMgr.isTopLabel()) {
    std::unique_ptr<ValueEntry> Val = nullptr;
    StackMgr.pop(Val);
    Vals.push_back(std::move(Val));
  }

  /// Pop label entry and the corresponding instruction sequence.
  InstrPdr.popInstrs();
  StackMgr.pop();

  /// Push the Vals back into the Stack
  for (auto Iter = Vals.rbegin(); Iter != Vals.rend(); Iter++)
    StackMgr.push(*Iter);
  return ErrCode::Success;
}

ErrCode Worker::invokeFunction(unsigned int FuncAddr) {
  ErrCode Status = ErrCode::Success;

  /// Get Function Instance and module address.
  Instance::FunctionInstance *FuncInst = nullptr;
  Instance::ModuleInstance *ModuleInst = nullptr;
  if ((Status = StoreMgr.getFunction(FuncAddr, FuncInst)) != ErrCode::Success)
    return Status;
  if ((StoreMgr.getModule(FuncInst->getModuleAddr(), ModuleInst)) !=
      ErrCode::Success)
    return Status;

  /// Get function type
  Instance::ModuleInstance::FType *FuncType = nullptr;
  if ((ModuleInst->getFuncType(FuncInst->getTypeIdx(), FuncType)) !=
      ErrCode::Success)
    return Status;

  /// Pop argument vals
  std::vector<std::unique_ptr<ValueEntry>> Vals;
  for (unsigned int I = 0; I < FuncType->Params.size(); I++) {
    std::unique_ptr<ValueEntry> Val;
    StackMgr.pop(Val);
    Vals.push_back(std::move(Val));
  }

  /// Push frame with locals and args and set instruction vector
  unsigned int Arity = FuncType->Returns.size();
  InstrVec EmprySeq;
  auto Frame =
      std::make_unique<FrameEntry>(FuncInst->getModuleAddr(), /// Module address
                                   Arity,                     /// Arity
                                   Vals,                 /// Reversed arguments
                                   FuncInst->getLocals() /// Local defs
      );
  StackMgr.push(Frame);
  InstrPdr.pushInstrs(InstrProvider::SeqType::FunctionCall, EmprySeq);

  /// Run block of function body
  return enterBlock(Arity, nullptr, FuncInst->getInstrs());
}

ErrCode Worker::returnFunction() {
  /// Get current frame and arity.
  StackMgr.getCurrentFrame(CurrentFrame);
  unsigned int Arity = CurrentFrame->getArity();

  /// Pop the results from stack.
  std::vector<std::unique_ptr<ValueEntry>> Vals;
  for (unsigned int I = 0; I < Arity; I++) {
    std::unique_ptr<ValueEntry> Val;
    StackMgr.pop(Val);
    Vals.push_back(std::move(Val));
  }

  /// TODO: Validate top of stack is a frame when reach end of function.

  /// Pop until the top of stack is a frame.
  while (!StackMgr.isTopFrame()) {
    /// If pop a label, need to pop the instruction sequence of block.
    if (StackMgr.isTopLabel())
      InstrPdr.popInstrs();
    StackMgr.pop();
  }

  /// Pop the frame entry from the Stack.
  InstrPdr.popInstrs();
  StackMgr.pop();

  /// Push the retrun Vals into Stack.
  for (auto Iter = Vals.rbegin(); Iter != Vals.rend(); Iter++) {
    std::unique_ptr<ValueEntry> Val = std::move(*Iter);
    StackMgr.push(Val);
  }
  return ErrCode::Success;
}

} // namespace Executor
} // namespace SSVM
