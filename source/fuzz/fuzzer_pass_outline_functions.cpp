// Copyright (c) 2019 Google LLC
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

#include "source/fuzz/fuzzer_pass_outline_functions.h"

#include <vector>

#include "source/fuzz/fuzzer_util.h"
#include "source/fuzz/instruction_descriptor.h"
#include "source/fuzz/transformation_add_loop_preheader.h"
#include "source/fuzz/transformation_outline_function.h"
#include "source/fuzz/transformation_split_block.h"

namespace spvtools {
namespace fuzz {

FuzzerPassOutlineFunctions::FuzzerPassOutlineFunctions(
    opt::IRContext* ir_context, TransformationContext* transformation_context,
    FuzzerContext* fuzzer_context,
    protobufs::TransformationSequence* transformations)
    : FuzzerPass(ir_context, transformation_context, fuzzer_context,
                 transformations) {}

FuzzerPassOutlineFunctions::~FuzzerPassOutlineFunctions() = default;

void FuzzerPassOutlineFunctions::Apply() {
  std::vector<opt::Function*> original_functions;
  for (auto& function : *GetIRContext()->module()) {
    original_functions.push_back(&function);
  }
  for (auto& function : original_functions) {
    if (!GetFuzzerContext()->ChoosePercentage(
            GetFuzzerContext()->GetChanceOfOutliningFunction())) {
      continue;
    }
    std::vector<opt::BasicBlock*> blocks;
    for (auto& block : *function) {
      blocks.push_back(&block);
    }
    auto entry_block = blocks[GetFuzzerContext()->RandomIndex(blocks)];

    // If the entry block is the header of a loop with a preheader, make the
    // preheader the new entry block. We need a while loop because the preheader
    // may itself be the header of another loop and have a preheader.
    while (auto preheader = fuzzerutil::MaybeFindLoopPreheader(
               GetIRContext(), entry_block->GetLabel()->result_id())) {
      entry_block = preheader;
    }

    // Check whether the entry block is still a loop header (with no
    // corresponding preheader).
    if (entry_block->IsLoopHeader()) {
      auto predecessors =
          GetIRContext()->cfg()->preds(entry_block->GetLabel()->result_id());

      if (predecessors.size() < 2) {
        // The header only has one predecessor (the back-edge block) and thus
        // it is unreachable.
        continue;
      }

      // The header has more than one out-of-loop predecessor. We need to add a
      // preheader for the loop.
      // Get a fresh id for the preheader.
      uint32_t preheader_id = GetFuzzerContext()->GetFreshId();

      // Get a fresh id for each OpPhi instruction.
      std::vector<uint32_t> phi_ids;
      entry_block->ForEachPhiInst(
          [this, &phi_ids](opt::Instruction* /* unused */) {
            phi_ids.push_back(GetFuzzerContext()->GetFreshId());
          });

      // Add the preheader.
      TransformationAddLoopPreheader(entry_block->GetLabel()->result_id(),
                                     preheader_id, phi_ids)
          .Apply(GetIRContext(), GetTransformationContext());

      // The entry block is now the preheader.
      entry_block = &*function->FindBlock(preheader_id);
    }

    // If the entry block starts with OpPhi or OpVariable, try to split it.
    if (entry_block->begin()->opcode() == SpvOpPhi ||
        entry_block->begin()->opcode() == SpvOpVariable) {
      // Find the first non-OpPhi and non-OpVariable instruction.
      opt::Instruction* non_phi_or_var_inst = nullptr;
      for (auto& instruction : *entry_block) {
        if (instruction.opcode() != SpvOpPhi &&
            instruction.opcode() != SpvOpVariable) {
          non_phi_or_var_inst = &instruction;
          break;
        }
      }

      assert(non_phi_or_var_inst &&
             "|non_phi_or_var_inst| must have been initialized");

      // If the split was not applicable, the transformation will not work.
      uint32_t new_block_id = GetFuzzerContext()->GetFreshId();
      if (!MaybeApplyTransformation(TransformationSplitBlock(
              MakeInstructionDescriptor(non_phi_or_var_inst->result_id(),
                                        non_phi_or_var_inst->opcode(), 0),
              new_block_id))) {
        continue;
      }

      // The new entry block is the newly-created block.
      entry_block = &*function->FindBlock(new_block_id);
    }

    auto dominator_analysis = GetIRContext()->GetDominatorAnalysis(function);
    auto postdominator_analysis =
        GetIRContext()->GetPostDominatorAnalysis(function);
    std::vector<opt::BasicBlock*> candidate_exit_blocks;
    for (auto postdominates_entry_block = entry_block;
         postdominates_entry_block != nullptr;
         postdominates_entry_block = postdominator_analysis->ImmediateDominator(
             postdominates_entry_block)) {
      // Consider the block if it is dominated by the entry block, ignore it if
      // it is a continue target.
      if (dominator_analysis->Dominates(entry_block,
                                        postdominates_entry_block) &&
          !GetIRContext()->GetStructuredCFGAnalysis()->IsContinueBlock(
              postdominates_entry_block->id())) {
        candidate_exit_blocks.push_back(postdominates_entry_block);
      }
    }
    if (candidate_exit_blocks.empty()) {
      continue;
    }
    auto exit_block = candidate_exit_blocks[GetFuzzerContext()->RandomIndex(
        candidate_exit_blocks)];

    // If the exit block is a merge block, try to split it and make the second
    // block in the pair become the exit block.
    if (GetIRContext()->GetStructuredCFGAnalysis()->IsMergeBlock(
            exit_block->id())) {
      uint32_t new_block_id = GetFuzzerContext()->GetFreshId();

      if (!MaybeApplyTransformation(TransformationSplitBlock(
              MakeInstructionDescriptor(exit_block->id(),
                                        exit_block->begin()->opcode(), 0),
              new_block_id))) {
        continue;
      }

      exit_block = &*function->FindBlock(new_block_id);
    }

    auto region_blocks = TransformationOutlineFunction::GetRegionBlocks(
        GetIRContext(), entry_block, exit_block);
    std::map<uint32_t, uint32_t> input_id_to_fresh_id;
    for (auto id : TransformationOutlineFunction::GetRegionInputIds(
             GetIRContext(), region_blocks, exit_block)) {
      input_id_to_fresh_id[id] = GetFuzzerContext()->GetFreshId();
    }
    std::map<uint32_t, uint32_t> output_id_to_fresh_id;
    for (auto id : TransformationOutlineFunction::GetRegionOutputIds(
             GetIRContext(), region_blocks, exit_block)) {
      output_id_to_fresh_id[id] = GetFuzzerContext()->GetFreshId();
    }
    TransformationOutlineFunction transformation(
        entry_block->id(), exit_block->id(),
        /*new_function_struct_return_type_id*/
        GetFuzzerContext()->GetFreshId(),
        /*new_function_type_id*/ GetFuzzerContext()->GetFreshId(),
        /*new_function_id*/ GetFuzzerContext()->GetFreshId(),
        /*new_function_region_entry_block*/
        GetFuzzerContext()->GetFreshId(),
        /*new_caller_result_id*/ GetFuzzerContext()->GetFreshId(),
        /*new_callee_result_id*/ GetFuzzerContext()->GetFreshId(),
        /*input_id_to_fresh_id*/ std::move(input_id_to_fresh_id),
        /*output_id_to_fresh_id*/ std::move(output_id_to_fresh_id));
    MaybeApplyTransformation(transformation);
  }
}

}  // namespace fuzz
}  // namespace spvtools
