// Copyright (c) 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "source/fuzz/transformation_flatten_conditional_branch.h"

#include "source/fuzz/fuzzer_util.h"
#include "source/fuzz/instruction_descriptor.h"

namespace spvtools {
namespace fuzz {

TransformationFlattenConditionalBranch::TransformationFlattenConditionalBranch(
    const protobufs::TransformationFlattenConditionalBranch& message)
    : message_(message) {}

TransformationFlattenConditionalBranch::TransformationFlattenConditionalBranch(
    uint32_t header_block_id,
    std::vector<
        std::pair<protobufs::InstructionDescriptor, std::vector<uint32_t>>>
        instructions_to_fresh_ids,
    std::vector<uint32_t> overflow_ids) {
  message_.set_header_block_id(header_block_id);
  for (auto const& pair : instructions_to_fresh_ids) {
    protobufs::InstructionUint32ListPair mapping;
    *mapping.mutable_instruction_descriptor() = pair.first;
    for (auto id : pair.second) {
      mapping.add_id(id);
    }
    *message_.add_instruction_to_fresh_ids() = mapping;
  }
  for (auto id : overflow_ids) {
    message_.add_overflow_id(id);
  }
}

bool TransformationFlattenConditionalBranch::IsApplicable(
    opt::IRContext* ir_context,
    const TransformationContext& /* unused */) const {
  uint32_t header_block_id = message_.header_block_id();
  auto header_block = fuzzerutil::MaybeFindBlock(ir_context, header_block_id);

  // The block must have been found and it must be a selection header.
  if (!header_block || !header_block->GetMergeInst() ||
      header_block->GetMergeInst()->opcode() != SpvOpSelectionMerge) {
    return false;
  }

  // The header block must end with an OpBranchConditional instruction.
  if (header_block->terminator()->opcode() != SpvOpBranchConditional) {
    return false;
  }

  // Use a set to keep track of the instructions that require fresh ids.
  std::set<opt::Instruction*> instructions_that_need_ids;

  // Check that, if there are enough ids, the conditional can be flattened.
  if (!ConditionalCanBeFlattened(ir_context, header_block,
                                 &instructions_that_need_ids)) {
    return false;
  }

  // Get the mapping from instructions to the fresh ids available for them.
  auto instructions_to_fresh_ids = GetInstructionsToFreshIdsMapping(ir_context);

  {
    // Check that all the ids given are fresh and distinct.

    std::set<uint32_t> used_fresh_ids;

    // Check the overflow ids.
    for (uint32_t id : message_.overflow_id()) {
      if (!CheckIdIsFreshAndNotUsedByThisTransformation(id, ir_context,
                                                        &used_fresh_ids)) {
        return false;
      }
    }

    // Check the ids in the map.
    for (const auto& pair : instructions_to_fresh_ids) {
      for (uint32_t id : pair.second) {
        if (!CheckIdIsFreshAndNotUsedByThisTransformation(id, ir_context,
                                                          &used_fresh_ids)) {
          return false;
        }
      }
    }
  }

  // Keep track of the number of overflow ids still available in the overflow
  // pool, as we go through the instructions.
  int remaining_overflow_ids = message_.overflow_id_size();

  for (auto instruction : instructions_that_need_ids) {
    // The number of ids needed depends on the id of the instruction.
    uint32_t ids_needed_by_this_instruction =
        NumOfFreshIdsNeededByInstruction(ir_context, *instruction);
    if (instructions_to_fresh_ids.count(instruction) != 0) {
      // If there is a mapping from this instruction to a list of fresh
      // ids, the list must have enough ids.

      if (instructions_to_fresh_ids[instruction].size() <
          ids_needed_by_this_instruction) {
        return false;
      }
    } else {
      // If there is no mapping, we need to rely on the pool of
      // overflow ids, where there must be enough remaining ids.

      remaining_overflow_ids -= ids_needed_by_this_instruction;

      if (remaining_overflow_ids < 0) {
        return false;
      }
    }
  }

  // All checks were passed.
  return true;
}

void TransformationFlattenConditionalBranch::Apply(
    opt::IRContext* ir_context,
    TransformationContext* transformation_context) const {
  uint32_t header_block_id = message_.header_block_id();
  auto header_block = ir_context->cfg()->block(header_block_id);

  // Find the first block where flow converges (it is not necessarily the merge
  // block).
  uint32_t convergence_block_id = header_block->MergeBlockId();
  while (ir_context->cfg()->preds(convergence_block_id).size() == 1) {
    convergence_block_id = ir_context->cfg()->preds(convergence_block_id)[0];
  }

  // Get the mapping from instructions to fresh ids.
  auto instructions_to_fresh_ids = GetInstructionsToFreshIdsMapping(ir_context);

  // Keep track of the number of overflow ids used.
  uint32_t overflow_ids_used = 0;

  auto branch_instruction = header_block->terminator();

  opt::BasicBlock* last_true_block = nullptr;

  // Adjust the conditional branches by enclosing problematic instructions
  // within conditionals and get references to the last block in each branch.
  for (int branch = 2; branch >= 1; branch--) {
    // branch = 1 corresponds to the true branch, branch = 2 corresponds to the
    // false branch. Consider the false branch first so that the true branch is
    // laid out right after the header.

    auto block = header_block;
    // Get the id of the first block in this branch.
    uint32_t block_id = branch_instruction->GetSingleWordInOperand(branch);

    // Consider all blocks in the branch until the convergence block is reached.
    while (block_id != convergence_block_id) {
      // Move the block to right after the previous one.
      block->GetParent()->MoveBasicBlockToAfter(block_id, block);

      block = ir_context->cfg()->block(block_id);
      block_id = block->terminator()->GetSingleWordInOperand(0);

      // Find all the instructions in the block which need to be enclosed inside
      // conditionals.
      std::vector<opt::Instruction*> problematic_instructions;

      block->ForEachInst(
          [&problematic_instructions](opt::Instruction* instruction) {
            if (instruction->opcode() != SpvOpLabel &&
                instruction->opcode() != SpvOpBranch &&
                !fuzzerutil::InstructionHasNoSideEffects(*instruction)) {
              problematic_instructions.push_back(instruction);
            }
          });

      uint32_t condition_id =
          header_block->terminator()->GetSingleWordInOperand(0);

      // Enclose all of the problematic instructions in conditionals, with the
      // same condition as the selection construct being flattened.
      for (auto instruction : problematic_instructions) {
        // Collect the fresh ids needed by this instructions
        uint32_t ids_needed =
            NumOfFreshIdsNeededByInstruction(ir_context, *instruction);
        std::vector<uint32_t> fresh_ids;

        // Get them from the map.
        if (instructions_to_fresh_ids.count(instruction) != 0) {
          fresh_ids = instructions_to_fresh_ids[instruction];
        }

        // If we could not get it from the map, use overflow ids.
        if (fresh_ids.empty()) {
          for (uint32_t i = 0; i < ids_needed; i++) {
            fresh_ids.push_back(message_.overflow_id(overflow_ids_used++));
          }
        }

        // Enclose the instruction in a conditional and get the merge block
        // generated by this operation (this is where all the next instructions
        // will be).
        block = EncloseInstructionInConditional(
            ir_context, transformation_context, block, instruction, fresh_ids,
            condition_id, branch == 1);
      }

      // If the next block is the convergence block and this is the true branch,
      // record this as the last block in the true branch.
      if (block_id == convergence_block_id && branch == 1) {
        last_true_block = block;
      }
    }
  }

  // Get the condition operand and the ids of the first blocks of the true and
  // false branches.
  auto condition_operand = branch_instruction->GetInOperand(0);
  uint32_t first_true_block_id = branch_instruction->GetSingleWordInOperand(1);
  uint32_t first_false_block_id = branch_instruction->GetSingleWordInOperand(2);

  // The current header should unconditionally branch to the first block in the
  // true branch, if there exists a true branch, and to the first block in the
  // false branch if there is no true branch.
  uint32_t after_header = first_true_block_id != convergence_block_id
                              ? first_true_block_id
                              : first_false_block_id;

  // Kill the merge instruction and the branch instruction in the current
  // header.
  auto merge_inst = header_block->GetMergeInst();
  ir_context->KillInst(branch_instruction);
  ir_context->KillInst(merge_inst);

  // Add a new, unconditional, branch instruction from the current header to
  // |after_header|.
  header_block->AddInstruction(MakeUnique<opt::Instruction>(
      ir_context, SpvOpBranch, 0, 0,
      opt::Instruction::OperandList{{SPV_OPERAND_TYPE_ID, {after_header}}}));

  // If there is a true branch, change the branch instruction so that the last
  // block in the true branch unconditionally branches to the first block in the
  // false branch (or the convergence block if there is no false branch).
  if (last_true_block) {
    last_true_block->terminator()->SetInOperand(0, {first_false_block_id});
  }

  // Replace all of the current OpPhi instructions in the convergence block with
  // OpSelect.
  ir_context->get_instr_block(convergence_block_id)
      ->ForEachPhiInst([&condition_operand](opt::Instruction* phi_inst) {
        phi_inst->SetOpcode(SpvOpSelect);
        std::vector<opt::Operand> operands;
        operands.emplace_back(condition_operand);
        // Only consider the operands referring to the instructions ids, as the
        // block labels are not necessary anymore.
        for (uint32_t i = 0; i < phi_inst->NumInOperands(); i += 2) {
          operands.emplace_back(phi_inst->GetInOperand(i));
        }
        phi_inst->SetInOperands(std::move(operands));
      });

  // Invalidate all analyses
  ir_context->InvalidateAnalysesExceptFor(opt::IRContext::kAnalysisNone);
}

protobufs::Transformation TransformationFlattenConditionalBranch::ToMessage()
    const {
  protobufs::Transformation result;
  *result.mutable_flatten_conditional_branch() = message_;
  return result;
}

bool TransformationFlattenConditionalBranch::ConditionalCanBeFlattened(
    opt::IRContext* ir_context, opt::BasicBlock* header,
    std::set<opt::Instruction*>* instructions_that_need_ids) {
  uint32_t merge_block_id = header->MergeBlockIdIfAny();
  assert(merge_block_id &&
         header->GetMergeInst()->opcode() == SpvOpSelectionMerge &&
         header->terminator()->opcode() == SpvOpBranchConditional &&
         "|header| must be the header of a conditional.");

  // Find the first block where flow converges (it is not necessarily the merge
  // block).
  uint32_t convergence_block_id = merge_block_id;
  while (ir_context->cfg()->preds(convergence_block_id).size() == 1) {
    if (convergence_block_id == header->id()) {
      // There is a chain of blocks with one predecessor from the header block
      // to the merge block. This means that the region is not single-entry,
      // single-exit (because the merge block is only reached by one of the two
      // branches).
      return false;
    }
    convergence_block_id = ir_context->cfg()->preds(convergence_block_id)[0];
  }

  auto enclosing_function = header->GetParent();
  auto dominator_analysis =
      ir_context->GetDominatorAnalysis(enclosing_function);
  auto postdominator_analysis =
      ir_context->GetPostDominatorAnalysis(enclosing_function);

  // Check that this is a single-entry, single-exit region, by checking that the
  // header dominates the convergence block and that the convergence block
  // post-dominates the header.
  if (!dominator_analysis->Dominates(header->id(), convergence_block_id) ||
      !postdominator_analysis->Dominates(convergence_block_id, header->id())) {
    return false;
  }

  // Traverse the CFG starting from the header and check that, for all the
  // blocks that can be reached by the header before reaching the convergence
  // block:
  //  - they don't contain merge, barrier or OpSampledImage instructions
  //  - they branch unconditionally to another block
  //  Add any side-effecting instruction, requiring fresh ids, to
  //  |instructions_that_need_ids|
  std::list<uint32_t> to_check;
  header->ForEachSuccessorLabel(
      [&to_check](uint32_t label) { to_check.push_back(label); });

  while (!to_check.empty()) {
    uint32_t block_id = to_check.front();
    to_check.pop_front();

    if (block_id == convergence_block_id) {
      // We have reached the convergence block, we don't need to consider its
      // successors.
      continue;
    }

    auto block = ir_context->cfg()->block(block_id);

    // The block must not have a merge instruction, because inner constructs are
    // not allowed.
    if (block->GetMergeInst()) {
      return false;
    }

    // Check all of the instructions in the block.
    bool all_instructions_compatible =
        block->WhileEachInst([ir_context, instructions_that_need_ids](
                                 opt::Instruction* instruction) {
          // We can ignore OpLabel instructions.
          if (instruction->opcode() == SpvOpLabel) {
            return true;
          }

          // If the instruction is a branch, it must be an unconditional branch.
          if (instruction->IsBranch()) {
            return instruction->opcode() == SpvOpBranch;
          }

          // We cannot go ahead if we encounter an instruction that cannot be
          // handled.
          if (!InstructionCanBeHandled(ir_context, *instruction)) {
            return false;
          }

          // If the instruction has side effects, add it to the
          // |instructions_that_need_ids| set.
          if (!fuzzerutil::InstructionHasNoSideEffects(*instruction)) {
            instructions_that_need_ids->emplace(instruction);
          }

          return true;
        });

    if (!all_instructions_compatible) {
      return false;
    }

    // Add the successor of this block to the list of blocks that need to be
    // checked.
    to_check.push_back(block->terminator()->GetSingleWordInOperand(0));
  }

  // All the blocks are compatible with the transformation and this is indeed a
  // single-entry, single-exit region.
  return true;
}

uint32_t
TransformationFlattenConditionalBranch::NumOfFreshIdsNeededByInstruction(
    opt::IRContext* ir_context, const opt::Instruction& instruction) {
  if (instruction.HasResultId()) {
    // We need 5 ids if the type returned is not Void, 2 otherwise.
    auto type = ir_context->get_type_mgr()->GetType(instruction.type_id());
    return (type && type->AsVoid()) ? 2 : 5;
  } else {
    return 2;
  }
}

std::unordered_map<opt::Instruction*, std::vector<uint32_t>>
TransformationFlattenConditionalBranch::GetInstructionsToFreshIdsMapping(
    opt::IRContext* ir_context) const {
  std::unordered_map<opt::Instruction*, std::vector<uint32_t>>
      instructions_to_fresh_ids;
  for (const auto& pair : message_.instruction_to_fresh_ids()) {
    std::vector<uint32_t> fresh_ids;
    for (uint32_t id : pair.id()) {
      fresh_ids.push_back(id);
    }

    auto instruction =
        FindInstruction(pair.instruction_descriptor(), ir_context);
    if (instruction) {
      instructions_to_fresh_ids.emplace(instruction, std::move(fresh_ids));
    }
  }

  return instructions_to_fresh_ids;
}

opt::BasicBlock*
TransformationFlattenConditionalBranch::EncloseInstructionInConditional(
    opt::IRContext* ir_context, TransformationContext* transformation_context,
    opt::BasicBlock* block, opt::Instruction* instruction,
    const std::vector<uint32_t>& fresh_ids, uint32_t condition_id,
    bool exec_if_cond_true) const {
  // Get the next instruction (it will be useful for splitting).
  auto next_instruction = instruction->NextNode();

  auto fresh_ids_needed =
      NumOfFreshIdsNeededByInstruction(ir_context, *instruction);

  // We must have enough fresh ids.
  assert(fresh_ids.size() >= fresh_ids_needed && "Not enough fresh ids.");

  // Update the module id bound
  for (auto id : fresh_ids) {
    fuzzerutil::UpdateModuleIdBound(ir_context, id);
  }

  // Create the block where the instruction is executed by splitting the
  // original block.
  auto execute_block = block->SplitBasicBlock(
      ir_context, fresh_ids[0],
      fuzzerutil::GetIteratorForInstruction(block, instruction));

  // Create the merge block for the conditional that we are about to create by
  // splitting execute_block (this will leave |instruction| as the only
  // instruction in |execute_block|).
  auto merge_block = execute_block->SplitBasicBlock(
      ir_context, fresh_ids[1],
      fuzzerutil::GetIteratorForInstruction(execute_block, next_instruction));

  // Propagate the fact that the block is dead to the newly-created blocks.
  if (transformation_context->GetFactManager()->BlockIsDead(block->id())) {
    transformation_context->GetFactManager()->AddFactBlockIsDead(
        execute_block->id());
    transformation_context->GetFactManager()->AddFactBlockIsDead(
        merge_block->id());
  }

  // Initially, consider the merge block as the alternative block to branch to
  // if the instruction should not be executed.
  auto alternative_block = merge_block;

  // Add an unconditional branch from |execute_block| to |merge_block|.
  execute_block->AddInstruction(MakeUnique<opt::Instruction>(
      ir_context, SpvOpBranch, 0, 0,
      opt::Instruction::OperandList{
          {SPV_OPERAND_TYPE_ID, {merge_block->id()}}}));

  // If the instruction requires 5 fresh ids, it means that it has a result id
  // and its result needs to be used later on, and we need to:
  // - add an additional block where a placeholder result is obtained by using
  //   the OpUndef instruction
  // - change the result id of the instruction to a fresh id
  // - add an OpPhi instruction, which will have the original result id of the
  //   instruction, in the merge block.
  if (NumOfFreshIdsNeededByInstruction(ir_context, *instruction) == 5) {
    // Create a new block using a fresh id for its label.
    auto alternative_block_temp = MakeUnique<opt::BasicBlock>(
        MakeUnique<opt::Instruction>(ir_context, SpvOpLabel, 0, fresh_ids[2],
                                     opt::Instruction::OperandList{}));

    // Keep the original result id of the instruction in a variable.
    uint32_t original_result_id = instruction->result_id();

    // Set the result id of the instruction to a fresh id.
    instruction->SetResultId(fresh_ids[3]);

    // Add an OpUndef instruction, with the same type as the original
    // instruction and a fresh id, to the new block.
    alternative_block_temp->AddInstruction(MakeUnique<opt::Instruction>(
        ir_context, SpvOpUndef, instruction->type_id(), fresh_ids[4],
        opt::Instruction::OperandList{}));

    // Add an unconditional branch from the new block to the merge block.
    alternative_block_temp->AddInstruction(MakeUnique<opt::Instruction>(
        ir_context, SpvOpBranch, 0, 0,
        opt::Instruction::OperandList{
            {SPV_OPERAND_TYPE_ID, {merge_block->id()}}}));

    // Insert the block before the merge block.
    alternative_block = block->GetParent()->InsertBasicBlockBefore(
        std::move(alternative_block_temp), merge_block);

    // Using the original instruction result id, add an OpPhi instruction to the
    // merge block, which will either take the value of the result of the
    // instruction or the dummy value defined in the alternative block.
    merge_block->begin().InsertBefore(MakeUnique<opt::Instruction>(
        ir_context, SpvOpPhi, instruction->type_id(), original_result_id,
        opt::Instruction::OperandList{
            {SPV_OPERAND_TYPE_ID, {instruction->result_id()}},
            {SPV_OPERAND_TYPE_ID, {execute_block->id()}},
            {SPV_OPERAND_TYPE_ID, {fresh_ids[4]}},
            {SPV_OPERAND_TYPE_ID, {alternative_block->id()}}}));

    // Propagate the fact that the block is dead to the new block.
    if (transformation_context->GetFactManager()->BlockIsDead(block->id())) {
      transformation_context->GetFactManager()->AddFactBlockIsDead(
          alternative_block->id());
    }
  }

  // Depending on whether the instruction should be executed in the if branch or
  // in the else branch, get the corresponding ids.
  auto if_block_id = (exec_if_cond_true ? execute_block : alternative_block)
                         ->GetLabel()
                         ->result_id();
  auto else_block_id = (exec_if_cond_true ? alternative_block : execute_block)
                           ->GetLabel()
                           ->result_id();

  // Add an OpSelectionMerge instruction to the block.
  block->AddInstruction(MakeUnique<opt::Instruction>(
      ir_context, SpvOpSelectionMerge, 0, 0,
      opt::Instruction::OperandList{{SPV_OPERAND_TYPE_ID, {merge_block->id()}},
                                    {SPV_OPERAND_TYPE_SELECTION_CONTROL,
                                     {SpvSelectionControlMaskNone}}}));

  // Add an OpBranchConditional, to the block, using |condition_id| as the
  // condition and branching to |if_block_id| if the condition is true and to
  // |else_block_id| if the condition is false.
  block->AddInstruction(MakeUnique<opt::Instruction>(
      ir_context, SpvOpBranchConditional, 0, 0,
      opt::Instruction::OperandList{{SPV_OPERAND_TYPE_ID, {condition_id}},
                                    {SPV_OPERAND_TYPE_ID, {if_block_id}},
                                    {SPV_OPERAND_TYPE_ID, {else_block_id}}}));

  return merge_block;
}

bool TransformationFlattenConditionalBranch::InstructionCanBeHandled(
    opt::IRContext* ir_context, const opt::Instruction& instruction) {
  // We can handle all instructions with no side effects.
  if (fuzzerutil::InstructionHasNoSideEffects(instruction)) {
    return true;
  }

  // We cannot handle barrier instructions, while we should be able to handle
  // all other instructions by enclosing them inside a conditional.
  if (instruction.opcode() == SpvOpControlBarrier ||
      instruction.opcode() == SpvOpMemoryBarrier ||
      instruction.opcode() == SpvOpNamedBarrierInitialize ||
      instruction.opcode() == SpvOpMemoryNamedBarrier ||
      instruction.opcode() == SpvOpTypeNamedBarrier) {
    return false;
  }

  // We cannot handle OpSampledImage instructions, as they need to be in the
  // same block as their use.
  if (instruction.opcode() == SpvOpSampledImage) {
    return false;
  }

  // We cannot handle instructions with an id which return a void type, if the
  // result id is used in the module (e.g. a function call to a function that
  // returns nothing).
  if (instruction.HasResultId()) {
    auto type = ir_context->get_type_mgr()->GetType(instruction.type_id());
    assert(type && "The type should be found in the module");

    if (type->AsVoid() &&
        !ir_context->get_def_use_mgr()->WhileEachUse(
            instruction.result_id(),
            [](opt::Instruction* use_inst, uint32_t use_index) {
              // Return false if the id is used as an input operand.
              return use_index <
                     use_inst->NumOperands() - use_inst->NumInOperands();
            })) {
      return false;
    }
  }

  return true;
}

}  // namespace fuzz
}  // namespace spvtools
