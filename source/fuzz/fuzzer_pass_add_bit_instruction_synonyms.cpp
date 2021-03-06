// Copyright (c) 2020 André Perez Maselco
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

#include "source/fuzz/fuzzer_pass_add_bit_instruction_synonyms.h"

#include "source/fuzz/fuzzer_util.h"
#include "source/fuzz/instruction_descriptor.h"
#include "source/fuzz/transformation_add_bit_instruction_synonym.h"

namespace spvtools {
namespace fuzz {

FuzzerPassAddBitInstructionSynonyms::FuzzerPassAddBitInstructionSynonyms(
    opt::IRContext* ir_context, TransformationContext* transformation_context,
    FuzzerContext* fuzzer_context,
    protobufs::TransformationSequence* transformations)
    : FuzzerPass(ir_context, transformation_context, fuzzer_context,
                 transformations) {}

FuzzerPassAddBitInstructionSynonyms::~FuzzerPassAddBitInstructionSynonyms() =
    default;

void FuzzerPassAddBitInstructionSynonyms::Apply() {
  for (auto& function : *GetIRContext()->module()) {
    for (auto& block : function) {
      for (auto& instruction : block) {
        // Randomly decides whether the transformation will be applied.
        if (!GetFuzzerContext()->ChoosePercentage(
                GetFuzzerContext()->GetChanceOfAddingBitInstructionSynonym())) {
          continue;
        }

        // TODO(https://github.com/KhronosGroup/SPIRV-Tools/issues/3557):
        //  Right now we only support certain operations. When this issue is
        //  addressed the following conditional can use the function
        //  |spvOpcodeIsBit|.
        if (instruction.opcode() != SpvOpBitwiseOr &&
            instruction.opcode() != SpvOpBitwiseXor &&
            instruction.opcode() != SpvOpBitwiseAnd &&
            instruction.opcode() != SpvOpNot) {
          continue;
        }

        // Right now, only integer operands are supported.
        if (GetIRContext()
                ->get_type_mgr()
                ->GetType(instruction.type_id())
                ->AsVector()) {
          continue;
        }

        // Make sure all bit indexes are defined as 32-bit unsigned integers.
        uint32_t width = GetIRContext()
                             ->get_type_mgr()
                             ->GetType(instruction.type_id())
                             ->AsInteger()
                             ->width();
        for (uint32_t i = 0; i < width; i++) {
          FindOrCreateIntegerConstant({i}, 32, false, false);
        }

        // Applies the add bit instruction synonym transformation.
        ApplyTransformation(TransformationAddBitInstructionSynonym(
            instruction.result_id(),
            GetFuzzerContext()->GetFreshIds(
                TransformationAddBitInstructionSynonym::GetRequiredFreshIdCount(
                    GetIRContext(), &instruction))));
      }
    }
  }
}

}  // namespace fuzz
}  // namespace spvtools
