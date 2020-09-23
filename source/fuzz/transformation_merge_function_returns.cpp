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

#include "source/fuzz/transformation_merge_function_returns.h"

#include "source/fuzz/comparator_deep_blocks_first.h"
#include "source/fuzz/fuzzer_util.h"

namespace spvtools {
namespace fuzz {

TransformationMergeFunctionReturns::TransformationMergeFunctionReturns(
    const protobufs::TransformationMergeFunctionReturns& message)
    : message_(message) {}

TransformationMergeFunctionReturns::TransformationMergeFunctionReturns(
    uint32_t function_id, uint32_t outer_header_id, uint32_t outer_return_id,
    uint32_t return_val_id, uint32_t any_returnable_val_id,
    const std::vector<protobufs::ReturnMergingInfo>& returns_merging_info) {
  message_.set_function_id(function_id);
  message_.set_outer_header_id(outer_header_id);
  message_.set_outer_return_id(outer_return_id);
  message_.set_return_val_id(return_val_id);
  message_.set_any_returnable_val_id(any_returnable_val_id);
  for (const auto& return_merging_info : returns_merging_info) {
    *message_.add_return_merging_info() = return_merging_info;
  }
}

bool TransformationMergeFunctionReturns::IsApplicable(
    opt::IRContext* ir_context,
    const TransformationContext& transformation_context) const {
  auto function = ir_context->GetFunction(message_.function_id());
  // The function must exist.
  if (!function) {
    return false;
  }

  // The entry block must end in an unconditional branch.
  if (function->entry()->terminator()->opcode() != SpvOpBranch) {
    return false;
  }

  // If the function has a non-void return type,
  // |message_.any_returnable_val_id| must exist, have the same type as the
  // return type of the function and be available at the end of the entry block.
  auto function_type = ir_context->get_type_mgr()->GetType(function->type_id());
  assert(function_type && "The function type should always exist.");

  // Get a map from the types for which ids are available at the end of the
  // entry block to one of the ids with that type. We compute this here to avoid
  // potentially doing it multiple times later on.
  auto types_to_available_ids =
      GetTypesToIdAvailableAfterEntryBlock(ir_context);

  if (!function_type->AsVoid()) {
    auto returnable_val_def =
        ir_context->get_def_use_mgr()->GetDef(message_.any_returnable_val_id());
    if (!returnable_val_def) {
      // Check if a suitable id can be found in the module.
      if (types_to_available_ids.count(function->type_id()) == 0) {
        return false;
      }
    } else if (returnable_val_def->type_id() != function->type_id()) {
      return false;
    } else if (!fuzzerutil::IdIsAvailableBeforeInstruction(
                   ir_context, function->entry()->terminator(),
                   message_.any_returnable_val_id())) {
      // The id must be available at the end of the entry block.
      return false;
    }
  }

  // Get the reachable return blocks.
  auto return_blocks =
      fuzzerutil::GetReachableReturnBlocks(ir_context, message_.function_id());

  // Get all the merge blocks of loops containing reachable return blocks.
  std::set<uint32_t> merge_blocks;
  for (uint32_t block : return_blocks) {
    uint32_t merge_block =
        ir_context->GetStructuredCFGAnalysis()->LoopMergeBlock(block);
    while (merge_block != 0 && !merge_blocks.count(merge_block)) {
      merge_blocks.emplace(merge_block);
      merge_block =
          ir_context->GetStructuredCFGAnalysis()->LoopMergeBlock(block);
    }
  }

  // All of the relevant merge blocks must not contain instructions whose opcode
  // is not one of OpLabel, OpPhi or OpBranch.
  for (uint32_t merge_block : merge_blocks) {
    bool all_instructions_allowed =
        ir_context->get_instr_block(merge_block)
            ->WhileEachInst([](opt::Instruction* inst) {
              return inst->opcode() == SpvOpLabel ||
                     inst->opcode() == SpvOpPhi ||
                     inst->opcode() == SpvOpBranch;
            });
    if (!all_instructions_allowed) {
      return false;
    }
  }

  // The module must contain an OpConstantTrue instruction.
  if (!fuzzerutil::MaybeGetBoolConstant(ir_context, transformation_context,
                                        true, false)) {
    return false;
  }

  // The module must contain an OpConstantFalse instruction.
  if (!fuzzerutil::MaybeGetBoolConstant(ir_context, transformation_context,
                                        false, false)) {
    return false;
  }

  // Check that the fresh ids provided are fresh and distinct.
  std::set<uint32_t> used_fresh_ids;
  for (uint32_t id : {message_.outer_header_id(), message_.outer_return_id()}) {
    if (!id || !CheckIdIsFreshAndNotUsedByThisTransformation(id, ir_context,
                                                             &used_fresh_ids)) {
      return false;
    }
  }

  // Check the additional fresh id required if the function is not void.
  if (!function_type->AsVoid() &&
      (!message_.return_val_id() ||
       !CheckIdIsFreshAndNotUsedByThisTransformation(
           message_.return_val_id(), ir_context, &used_fresh_ids))) {
    return false;
  }

  auto merge_blocks_to_info = GetMappingOfMergeBlocksToInfo();

  // For each relevant merge block, check that the correct ids are available.
  for (uint32_t merge_block : merge_blocks) {
    // A map from OpPhi ids to ids of the same type available at the beginning
    // of the merge block.
    std::map<uint32_t, uint32_t> phi_to_id;

    if (merge_blocks_to_info.count(merge_block) > 0) {
      // If the map contains an entry for the merge block, check that the fresh
      // ids are fresh and distinct.
      auto info = merge_blocks_to_info[merge_block];
      if (!info.is_returning_id() ||
          !CheckIdIsFreshAndNotUsedByThisTransformation(
              info.is_returning_id(), ir_context, &used_fresh_ids)) {
        return false;
      }

      if (!function_type->AsVoid() &&
          (!info.maybe_return_val_id() ||
           !CheckIdIsFreshAndNotUsedByThisTransformation(
               info.maybe_return_val_id(), ir_context, &used_fresh_ids))) {
        return false;
      }

      // Get the mapping from OpPhis to suitable ids.
      phi_to_id = fuzzerutil::RepeatedUInt32PairToMap(
          *info.mutable_opphi_to_suitable_id());
    } else {
      // If the map does not contain an entry for the merge block, check that
      // overflow ids are available.
      if (!transformation_context.GetOverflowIdSource()->HasOverflowIds()) {
        return false;
      }
    }

    // For each OpPhi instruction, check that a suitable placeholder id is
    // available.
    bool suitable_info_for_phi =
        ir_context->get_instr_block(merge_block)
            ->WhileEachPhiInst([ir_context, &phi_to_id,
                                &types_to_available_ids](
                                   opt::Instruction* inst) {
              if (phi_to_id.count(inst->result_id()) > 0) {
                // If there exists a mapping for this instruction and the
                // placeholder id exists in the module, check that it has the
                // correct type and it is available before the instruction.
                auto placeholder_def = ir_context->get_def_use_mgr()->GetDef(
                    phi_to_id[inst->result_id()]);
                if (placeholder_def) {
                  if (inst->type_id() != placeholder_def->type_id()) {
                    return false;
                  }
                  if (!fuzzerutil::IdIsAvailableBeforeInstruction(
                          ir_context, inst, placeholder_def->result_id())) {
                    return false;
                  }

                  return true;
                }
              }

              // If there is no mapping, check if there is a suitable id
              // available at the end of the entry block.
              return types_to_available_ids.count(inst->type_id()) > 0;
            });

    if (!suitable_info_for_phi) {
      return false;
    }
  }

  return true;
}

void TransformationMergeFunctionReturns::Apply(
    opt::IRContext* ir_context,
    TransformationContext* transformation_context) const {
  auto function = ir_context->GetFunction(message_.function_id());
  auto function_type = ir_context->get_type_mgr()->GetType(function->type_id());

  // Get a map from the types for which ids are available at the end of the
  // entry block to one of the ids with that type. We compute this here to avoid
  // potentially doing it multiple times later on.
  auto types_to_available_ids =
      GetTypesToIdAvailableAfterEntryBlock(ir_context);

  // Get a reference to an instruction with the same type id as the function's
  // return type, if the type of the function is not void.
  uint32_t returnable_val_id = 0;
  if (!function_type->AsVoid()) {
    // If |message.any_returnable_val_id| can be found in the module, use it.
    // Otherwise, use another suitable id found in the module.
    auto returnable_val_def =
        ir_context->get_def_use_mgr()->GetDef(message_.any_returnable_val_id());
    returnable_val_id = returnable_val_def
                            ? returnable_val_def->result_id()
                            : types_to_available_ids[function->type_id()];
  }

  uint32_t bool_type = fuzzerutil::MaybeGetBoolType(ir_context);

  uint32_t constant_true = fuzzerutil::MaybeGetBoolConstant(
      ir_context, *transformation_context, true, false);

  uint32_t constant_false = fuzzerutil::MaybeGetBoolConstant(
      ir_context, *transformation_context, false, false);

  // Get the reachable return blocks.
  auto return_blocks =
      fuzzerutil::GetReachableReturnBlocks(ir_context, message_.function_id());

  // Keep a map from the relevant merge blocks to a mapping from each of the
  // returning predecessors to the corresponding pair (return value,
  // boolean specifying whether the function is returning). Returning
  // predecessors are blocks in the loop (not further nested inside loops),
  // which either return or are merge blocks of nested loops containing return
  // instructions.
  std::map<uint32_t, std::map<uint32_t, std::pair<uint32_t, uint32_t>>>
      merge_blocks_to_returning_predecessors;

  // Initialise the map, mapping each relevant merge block to an empty map.
  for (uint32_t ret_block_id : return_blocks) {
    uint32_t merge_block_id =
        ir_context->GetStructuredCFGAnalysis()->LoopMergeBlock(ret_block_id);

    while (merge_block_id != 0 &&
           !merge_blocks_to_returning_predecessors.count(merge_block_id)) {
      merge_blocks_to_returning_predecessors.emplace(
          merge_block_id, std::map<uint32_t, std::pair<uint32_t, uint32_t>>());
      merge_block_id =
          ir_context->GetStructuredCFGAnalysis()->LoopMergeBlock(ret_block_id);
    }
  }

  // Keep a map from all the new predecessors of the merge block of the new
  // outer loop, to the related return value ids.
  std::map<uint32_t, uint32_t> outer_merge_predecessors;

  // Adjust the return blocks and add the related information to the map or
  // |outer_merge_predecessors| set.
  for (uint32_t ret_block_id : return_blocks) {
    auto ret_block = ir_context->get_instr_block(ret_block_id);

    // Get the return value id (if the function is not void).
    uint32_t ret_val_id =
        function_type->AsVoid()
            ? 0
            : ret_block->terminator()->GetSingleWordInOperand(0);

    uint32_t merge_block_id =
        ir_context->GetStructuredCFGAnalysis()->LoopMergeBlock(ret_block_id);

    // Add a new entry to the map corresponding to the merge block of the
    // innermost enclosing loop (or that of the new outer loop if there is no
    // enclosing loop).
    if (merge_block_id != 0) {
      merge_blocks_to_returning_predecessors[merge_block_id].emplace(
          ret_block_id,
          std::pair<uint32_t, uint32_t>(ret_val_id, constant_true));
    } else {
      outer_merge_predecessors.emplace(ret_block_id, ret_val_id);
    }

    // If there is no enclosing loop, the block will branch to the merge block
    // of the new outer loop.
    merge_block_id = message_.outer_return_id();

    // Replace the return instruction with an unconditional branch.
    ret_block->terminator()->SetOpcode(SpvOpBranch);
    ret_block->terminator()->SetInOperands(
        {{SPV_OPERAND_TYPE_RESULT_ID, {merge_block_id}}});
  }

  // Get a list of all the relevant merge blocks.
  std::vector<uint32_t> merge_blocks(
      merge_blocks_to_returning_predecessors.size());
  for (const auto& entry : merge_blocks_to_returning_predecessors) {
    merge_blocks.emplace_back(entry.first);
  }
  // Sort the list so that deeper merge blocks come first.
  std::sort(merge_blocks.begin(), merge_blocks.end(),
            ComparatorDeepBlocksFirst(ir_context));

  auto merge_blocks_to_info = GetMappingOfMergeBlocksToInfo();

  // Adjust the merge blocks and add the related information to the map or
  // |outer_merge_predecessors| set.
  for (uint32_t merge_block_id : merge_blocks) {
    // Get the info corresponding to |merge_block| from the map, if a
    // corresponding entry exists. Otherwise use overflow ids and find suitable
    // ids in the module.
    protobufs::ReturnMergingInfo* info =
        merge_blocks_to_info.count(merge_block_id)
            ? &merge_blocks_to_info[merge_block_id]
            : nullptr;

    uint32_t is_returning_id =
        info ? info->is_returning_id()
             : transformation_context->GetOverflowIdSource()
                   ->GetNextOverflowId();

    uint32_t maybe_return_val_id = 0;
    if (!function_type->AsVoid()) {
      maybe_return_val_id = info ? info->maybe_return_val_id()
                                 : transformation_context->GetOverflowIdSource()
                                       ->GetNextOverflowId();
    }

    // Map from existing OpPhi to overflow ids. If there is no mapping, get an
    // empty map.
    auto phi_to_id = info ? fuzzerutil::RepeatedUInt32PairToMap(
                                *merge_blocks_to_info[merge_block_id]
                                     .mutable_opphi_to_suitable_id())
                          : std::map<uint32_t, uint32_t>();

    // Get a reference to the info related to the returning predecessors.
    const auto& returning_preds =
        merge_blocks_to_returning_predecessors[merge_block_id];

    // Get a set of the original predecessors.
    auto preds_list = ir_context->cfg()->preds(merge_block_id);
    auto preds = std::set<uint32_t>(preds_list.begin(), preds_list.end());

    auto merge_block = ir_context->get_instr_block(merge_block_id);

    // Adjust the existing OpPhi instructions.
    merge_block->ForEachPhiInst([&preds, &returning_preds, &phi_to_id,
                                 &types_to_available_ids](
                                    opt::Instruction* inst) {
      // We need a placeholder value id. If |phi_to_id| contains a mapping
      // for this instruction, we use the given id, otherwise a suitable id
      // for the instruction's type from |types_to_available_ids|.
      uint32_t placeholder_val_id =
          phi_to_id.count(inst->result_id())
              ? phi_to_id[inst->result_id()]
              : types_to_available_ids[inst->type_id()];
      assert(placeholder_val_id &&
             "We should always be able to find a suitable if the "
             "transformation is applicable.");

      // Add a pair of operands (placeholder id, new predecessor) for each
      // new predecessor of the merge block.
      for (const auto& entry : returning_preds) {
        // A returning predecessor may already be a predecessor of the
        // block. In that case, we should not add new operands.
        // Each entry is in the form (predecessor, {return val, is returning}).
        if (!preds.count(entry.first)) {
          inst->AddOperand({SPV_OPERAND_TYPE_RESULT_ID, {placeholder_val_id}});
          inst->AddOperand({SPV_OPERAND_TYPE_RESULT_ID, {entry.first}});
        }
      }
    });

    // If the function is not void, add a new OpPhi instructions to collect the
    // return value from the returning predecessors.
    if (!function_type->AsVoid()) {
      opt::Instruction::OperandList operand_list;

      // Add two operands (return value, predecessor) for each returning
      // predecessor.
      for (auto entry : returning_preds) {
        // Each entry is in the form (predecessor, {return value,
        // is returning}).
        operand_list.emplace_back(
            opt::Operand{SPV_OPERAND_TYPE_RESULT_ID, {entry.second.first}});
        operand_list.emplace_back(
            opt::Operand{SPV_OPERAND_TYPE_RESULT_ID, {entry.first}});
      }

      // Add two operands for each original predecessor from which the function
      // does not return.
      for (uint32_t original_pred : preds) {
        // Only add operands if the function cannot be returning from this
        // block.
        if (returning_preds.count(original_pred)) {
          continue;
        }

        operand_list.emplace_back(
            opt::Operand{SPV_OPERAND_TYPE_RESULT_ID, {returnable_val_id}});
        operand_list.emplace_back(
            opt::Operand{SPV_OPERAND_TYPE_RESULT_ID, {original_pred}});
      }

      // Insert the instruction.
      merge_block->begin()->InsertBefore(MakeUnique<opt::Instruction>(
          ir_context, SpvOpPhi, function->type_id(), maybe_return_val_id,
          std::move(operand_list)));

      fuzzerutil::UpdateModuleIdBound(ir_context, maybe_return_val_id);
    }

    // Add an OpPhi instruction deciding whether the function is returning.
    {
      opt::Instruction::OperandList operand_list;

      // Add two operands (return value, is returning) for each returning
      // predecessor.
      for (auto entry : returning_preds) {
        // Each entry is in the form (predecessor, {return value,
        // is returning}).
        operand_list.emplace_back(
            opt::Operand{SPV_OPERAND_TYPE_RESULT_ID, {entry.second.second}});
        operand_list.emplace_back(
            opt::Operand{SPV_OPERAND_TYPE_RESULT_ID, {entry.first}});
      }

      // Add two operands for each original predecessor from which the function
      // does not return.
      for (uint32_t original_pred : preds) {
        // Only add operands if the function cannot be returning from this
        // block.
        if (returning_preds.count(original_pred)) {
          continue;
        }

        operand_list.emplace_back(
            opt::Operand{SPV_OPERAND_TYPE_RESULT_ID, {constant_false}});
        operand_list.emplace_back(
            opt::Operand{SPV_OPERAND_TYPE_RESULT_ID, {original_pred}});
      }

      // Insert the instruction.
      merge_block->begin()->InsertBefore(MakeUnique<opt::Instruction>(
          ir_context, SpvOpPhi, bool_type, is_returning_id,
          std::move(operand_list)));

      fuzzerutil::UpdateModuleIdBound(ir_context, is_returning_id);
    }

    // Change the branching instruction of the block.
    assert(merge_block->terminator()->opcode() == SpvOpBranch &&
           "Each block should branch unconditionally to the next.");

    // Add a new entry to the map corresponding to the merge block of the
    // innermost enclosing loop (or that of the new outer loop if there is no
    // enclosing loop).
    uint32_t enclosing_merge =
        ir_context->GetStructuredCFGAnalysis()->LoopMergeBlock(merge_block_id);
    if (enclosing_merge == 0) {
      enclosing_merge = message_.outer_return_id();
      outer_merge_predecessors.emplace(merge_block_id, maybe_return_val_id);
    } else {
      merge_blocks_to_returning_predecessors[enclosing_merge].emplace(
          merge_block_id,
          std::pair<uint32_t, uint32_t>(maybe_return_val_id, is_returning_id));
    }

    // Get the current successor.
    uint32_t original_succ =
        merge_block->terminator()->GetSingleWordInOperand(0);
    // Leave the instruction as it is if the block already branches to the merge
    // block of the enclosing loop.
    if (original_succ == enclosing_merge) {
      continue;
    }

    // The block should branch to |enclosing_merge| if |is_returning_id| is
    // true, to |original_succ| otherwise.
    merge_block->terminator()->SetOpcode(SpvOpBranch);
    merge_block->terminator()->SetInOperands(
        {{SPV_OPERAND_TYPE_RESULT_ID, {is_returning_id}},
         {SPV_OPERAND_TYPE_RESULT_ID, {enclosing_merge}},
         {SPV_OPERAND_TYPE_RESULT_ID, {original_succ}}});
  }

  assert(function->entry()->terminator()->opcode() == SpvOpBranch &&
         "The entry block should branch unconditionally to another block.");
  uint32_t block_after_entry =
      function->entry()->terminator()->GetSingleWordInOperand(0);

  // Create the header for the new outer loop.
  auto outer_loop_header =
      MakeUnique<opt::BasicBlock>(MakeUnique<opt::Instruction>(
          ir_context, SpvOpLabel, 0, message_.outer_header_id(),
          opt::Instruction::OperandList()));

  fuzzerutil::UpdateModuleIdBound(ir_context, message_.outer_header_id());

  // Add the instruction: OpLoopMerge %outer_return_id %outer_header_id None
  // The header is the continue block of the outer loop.
  outer_loop_header->AddInstruction(MakeUnique<opt::Instruction>(
      ir_context, SpvOpLoopMerge, 0, 0,
      opt::Instruction::OperandList{
          {SPV_OPERAND_TYPE_RESULT_ID, {message_.outer_return_id()}},
          {SPV_OPERAND_TYPE_RESULT_ID, {message_.outer_header_id()}},
          {SPV_OPERAND_TYPE_LOOP_CONTROL, {SpvLoopControlMaskNone}}}));

  // Add conditional branch:
  // 	OpBranchConditional %true %block_after_entry %outer_header_id
  // This will always branch to %block_after_entry, but it also creates a back
  // edge for the loop (which is never traversed).
  outer_loop_header->AddInstruction(MakeUnique<opt::Instruction>(
      ir_context, SpvOpBranchConditional, 0, 0,
      opt::Instruction::OperandList{
          {SPV_OPERAND_TYPE_RESULT_ID, {constant_true}},
          {SPV_OPERAND_TYPE_RESULT_ID, {block_after_entry}},
          {SPV_OPERAND_TYPE_LOOP_CONTROL, {message_.outer_header_id()}}}));

  // Insert the header right after the entry block.
  function->InsertBasicBlockAfter(std::move(outer_loop_header),
                                  function->entry().get());

  // Update the branching instruction of the entry block.
  function->entry()->terminator()->SetInOperands(
      {{SPV_OPERAND_TYPE_RESULT_ID, {message_.outer_header_id()}}});

  // Create the merge block for the loop (and return block for the function).
  auto outer_return_block =
      MakeUnique<opt::BasicBlock>(MakeUnique<opt::Instruction>(
          ir_context, SpvOpLabel, 0, message_.outer_return_id(),
          opt::Instruction::OperandList()));

  fuzzerutil::UpdateModuleIdBound(ir_context, message_.outer_return_id());

  // If the function is not void, insert an instruction to collect the return
  // value from the predecessors and an OpReturnValue instruction.
  if (!function_type->AsVoid()) {
    opt::Instruction::OperandList operand_list;

    // Add two operands (return value, predecessor) for each predecessor.
    for (auto entry : outer_merge_predecessors) {
      // Each entry is in the form (predecessor, return value).
      operand_list.emplace_back(
          opt::Operand{SPV_OPERAND_TYPE_RESULT_ID, {entry.second}});
      operand_list.emplace_back(
          opt::Operand{SPV_OPERAND_TYPE_RESULT_ID, {entry.first}});
    }

    // Insert the OpPhi instruction.
    outer_return_block->AddInstruction(MakeUnique<opt::Instruction>(
        ir_context, SpvOpPhi, function->type_id(), message_.return_val_id(),
        std::move(operand_list)));

    fuzzerutil::UpdateModuleIdBound(ir_context, message_.return_val_id());

    // Insert the OpReturnValue instruction.
    outer_return_block->AddInstruction(MakeUnique<opt::Instruction>(
        ir_context, SpvOpReturnValue, 0, 0,
        opt::Instruction::OperandList{
            {SPV_OPERAND_TYPE_RESULT_ID, {message_.return_val_id()}}}));
  } else {
    // Insert an OpReturn instruction (the function is void).
    outer_return_block->AddInstruction(MakeUnique<opt::Instruction>(
        ir_context, SpvOpReturn, 0, 0, opt::Instruction::OperandList{}));
  }

  // Insert the new return block at the end of the function.
  function->AddBasicBlock(std::move(outer_return_block));

  // All analyses must be invalidated because the structure of the module was
  // changed.
  ir_context->InvalidateAnalysesExceptFor(opt::IRContext::kAnalysisNone);
}

protobufs::Transformation TransformationMergeFunctionReturns::ToMessage()
    const {
  return protobufs::Transformation();
}

std::map<uint32_t, protobufs::ReturnMergingInfo>
TransformationMergeFunctionReturns::GetMappingOfMergeBlocksToInfo() const {
  std::map<uint32_t, protobufs::ReturnMergingInfo> result;
  for (const auto& info : message_.return_merging_info()) {
    result.emplace(info.merge_block_id(), info);
  }
  return result;
}

std::map<uint32_t, uint32_t>
TransformationMergeFunctionReturns::GetTypesToIdAvailableAfterEntryBlock(
    opt::IRContext* ir_context) const {
  std::map<uint32_t, uint32_t> result;
  // Consider all global declarations
  for (auto& global : ir_context->module()->types_values()) {
    if (global.HasResultId() && global.type_id()) {
      result.emplace(global.type_id(), global.result_id());
    }
  }

  auto function = ir_context->GetFunction(message_.function_id());
  assert(function && "The function must exist.");

  // Consider all function parameters
  function->ForEachParam([&result](opt::Instruction* param) {
    if (param->HasResultId() && param->type_id()) {
      result.emplace(param->type_id(), param->result_id());
    }
  });

  // Consider all the instructions in the entry block.
  for (auto& inst : *function->entry()) {
    if (inst.HasResultId() && inst.type_id()) {
      result.emplace(inst.type_id(), inst.result_id());
    }
  }

  return result;
}

}  // namespace fuzz
}  // namespace spvtools
