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

#include "source/fuzz/fuzzer_pass_flatten_conditional_branches.h"

#include "source/fuzz/instruction_descriptor.h"
#include "source/fuzz/transformation_flatten_conditional_branch.h"

namespace spvtools {
namespace fuzz {

// A fuzzer pass that randomly selects conditional branches to flatten and
// flattens them, if possible.
FuzzerPassFlattenConditionalBranches::FuzzerPassFlattenConditionalBranches(
    opt::IRContext* ir_context, TransformationContext* transformation_context,
    FuzzerContext* fuzzer_context,
    protobufs::TransformationSequence* transformations)
    : FuzzerPass(ir_context, transformation_context, fuzzer_context,
                 transformations) {}

FuzzerPassFlattenConditionalBranches::~FuzzerPassFlattenConditionalBranches() =
    default;

void FuzzerPassFlattenConditionalBranches::Apply() {
  // Get all the selection headers that we want to flatten. We need to collect
  // all of them first, because, since we are changing the structure of the
  // module, it's not safe to modify them while iterating.
  std::vector<opt::BasicBlock*> selection_headers;
  for (auto& function : *GetIRContext()->module()) {
    for (auto& block : function) {
      // Randomly decide whether to consider this block.
      if (!GetFuzzerContext()->ChoosePercentage(
              GetFuzzerContext()->GetChanceOfFlatteningConditionalBranch())) {
        continue;
      }

      // Only consider this block if it is the header of a conditional.
      if (block.GetMergeInst() &&
          block.GetMergeInst()->opcode() == SpvOpSelectionMerge &&
          block.terminator()->opcode() == SpvOpBranchConditional) {
        selection_headers.emplace_back(&block);
      }
    }
  }

  // Apply the transformation to the headers which can be flattened.
  for (auto header : selection_headers) {
    // Make a set to keep track of the instructions that need fresh ids.
    std::set<opt::Instruction*> instructions_that_need_ids;

    // Do not consider this header if the conditional cannot be flattened.
    if (!TransformationFlattenConditionalBranch::ConditionalCanBeFlattened(
            GetIRContext(), header, &instructions_that_need_ids)) {
      continue;
    }

    // Generate entries (instruction descriptor, fresh ids) for all the
    // instructions that require fresh ids.
    std::vector<
        std::pair<protobufs::InstructionDescriptor, std::vector<uint32_t>>>
        instructions_to_fresh_ids;

    for (auto instruction : instructions_that_need_ids) {
      uint32_t num_fresh_ids_needed =
          TransformationFlattenConditionalBranch::NumOfFreshIdsNeededByOpcode(
              instruction->opcode());

      instructions_to_fresh_ids.push_back(
          {MakeInstructionDescriptor(GetIRContext(), instruction),
           GetFuzzerContext()->GetFreshIds(num_fresh_ids_needed)});
    }

    // Add 10 overflow ids to account for possible changes in the module.
    auto overflow_ids = GetFuzzerContext()->GetFreshIds(10);

    // Apply the transformation.
    ApplyTransformation(TransformationFlattenConditionalBranch(
        header->id(), std::move(instructions_to_fresh_ids),
        std::move(overflow_ids)));
  }
}
}  // namespace fuzz
}  // namespace spvtools