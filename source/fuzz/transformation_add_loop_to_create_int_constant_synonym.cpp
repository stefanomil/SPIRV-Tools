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

#include "source/fuzz/transformation_add_loop_to_create_int_constant_synonym.h"
#include "source/fuzz/fuzzer_util.h"

namespace spvtools {
namespace fuzz {

TransformationAddLoopToCreateIntConstantSynonym::
    TransformationAddLoopToCreateIntConstantSynonym(
        const protobufs::TransformationAddLoopToCreateIntConstantSynonym&
            message)
    : message_(message) {}

TransformationAddLoopToCreateIntConstantSynonym::
    TransformationAddLoopToCreateIntConstantSynonym(
        uint32_t constant_id, uint32_t initial_val_id, uint32_t step_val_id,
        uint32_t num_iterations_id, uint32_t block_after_loop_id,
        uint32_t syn_id, uint32_t loop_id, uint32_t ctr_id, uint32_t temp_id,
        uint32_t eventual_syn_id, uint32_t incremented_ctr_id, uint32_t cond_id,
        uint32_t additional_block_id) {
  message_.set_constant_id(constant_id);
  message_.set_initial_val_id(initial_val_id);
  message_.set_step_val_id(step_val_id);
  message_.set_num_iterations_id(num_iterations_id);
  message_.set_block_after_loop_id(block_after_loop_id);
  message_.set_syn_id(syn_id);
  message_.set_loop_id(loop_id);
  message_.set_ctr_id(ctr_id);
  message_.set_temp_id(temp_id);
  message_.set_eventual_syn_id(eventual_syn_id);
  message_.set_incremented_ctr_id(incremented_ctr_id);
  message_.set_cond_id(cond_id);
  message_.set_additional_block_id(additional_block_id);
}

bool TransformationAddLoopToCreateIntConstantSynonym::IsApplicable(
    opt::IRContext* ir_context,
    const TransformationContext& transformation_context) const {
  // Check that |message_.constant_id|, |message_.initial_val_id| and
  // |message_.step_val_id| are existing constants.

  auto constant = ir_context->get_constant_mgr()->FindDeclaredConstant(
      message_.constant_id());
  auto initial_val = ir_context->get_constant_mgr()->FindDeclaredConstant(
      message_.initial_val_id());
  auto step_val = ir_context->get_constant_mgr()->FindDeclaredConstant(
      message_.step_val_id());

  if (!constant || !initial_val || !step_val) {
    return false;
  }

  // Check that |constant| is an integer scalar or vector constant.
  if (!constant->AsIntConstant() &&
      (!constant->AsVectorConstant() ||
       !constant->type()->AsVector()->element_type()->AsInteger())) {
    return false;
  }

  // Check that the component bit width of |constant| is <= 64.
  // Consider the width of the constant if it is an integer, of a single
  // component if it is a vector.
  uint32_t bit_width =
      constant->AsIntConstant()
          ? constant->type()->AsInteger()->width()
          : constant->type()->AsVector()->element_type()->AsInteger()->width();
  if (bit_width > 64) {
    return false;
  }

  auto constant_def =
      ir_context->get_def_use_mgr()->GetDef(message_.constant_id());
  auto initial_val_def =
      ir_context->get_def_use_mgr()->GetDef(message_.initial_val_id());
  auto step_val_def =
      ir_context->get_def_use_mgr()->GetDef(message_.step_val_id());

  // Check that |constant|, |initial_val| and |step_val| have the same type,
  // with possibly different signedness.
  if (!fuzzerutil::TypesAreEqualUpToSign(ir_context, constant_def->type_id(),
                                         initial_val_def->type_id()) ||
      !fuzzerutil::TypesAreEqualUpToSign(ir_context, constant_def->type_id(),
                                         step_val_def->type_id())) {
    return false;
  }

  // |message_.num_iterations_id| is an integer constant with bit width 32.
  auto num_iterations_def =
      ir_context->get_def_use_mgr()->GetDef(message_.num_iterations_id());
  auto num_iterations = ir_context->get_constant_mgr()->FindDeclaredConstant(
      message_.num_iterations_id());

  if (!num_iterations || num_iterations->AsIntConstant() ||
      num_iterations->type()->AsInteger()->width() != 32) {
    return false;
  }

  // Check that the number of iterations is > 0 and <= 32.
  int32_t num_iterations_value =
      num_iterations->AsIntConstant()->GetS32BitValue();

  if (num_iterations_value <= 0 || num_iterations_value > 32) {
    return false;
  }

  // Check that the module contains 32-bit signed integer scalar constants of
  // value 0 and 1.
  if (!fuzzerutil::MaybeGetIntegerConstant(ir_context, transformation_context,
                                           {0}, 32, true, false)) {
    return false;
  }

  if (!fuzzerutil::MaybeGetIntegerConstant(ir_context, transformation_context,
                                           {1}, 32, true, false)) {
    return false;
  }

  // Check that the equation C = I - S * N is satisfied.

  // Collect the components in vectors (if the constants are scalars, these
  // vectors will contain the constants themselves).
  std::vector<const opt::analysis::Constant*> c_components;
  std::vector<const opt::analysis::Constant*> i_components;
  std::vector<const opt::analysis::Constant*> s_components;
  if (constant->AsIntConstant()) {
    c_components.emplace_back(constant);
    i_components.emplace_back(initial_val);
    s_components.emplace_back(step_val);
  } else {
    // It is a vector: get all the components.
    c_components = constant->AsVectorConstant()->GetComponents();
    i_components = initial_val->AsVectorConstant()->GetComponents();
    s_components = step_val->AsVectorConstant()->GetComponents();
  }

  // Check the value of the components satisfy the equation (the values are sign
  // extended because this way the equation is satisfied iff it would be
  // satisfied with the non-extended values).
  for (uint32_t i = 0; i < c_components.size(); i++) {
    int64_t c_value = c_components[i]->AsIntConstant()->GetSignExtendedValue();
    int64_t i_value = i_components[i]->AsIntConstant()->GetSignExtendedValue();
    int64_t s_value = s_components[i]->AsIntConstant()->GetSignExtendedValue();
    if (c_value != i_value - s_value * num_iterations_value) {
      return false;
    }
  }

  // Check that |message_.block_after_loop_id| is the label of a block.
  auto block =
      fuzzerutil::MaybeFindBlock(ir_context, message_.block_after_loop_id());

  // Check that the block has a single predecessor.
  if (ir_context->cfg()->preds(block->id()).size() != 1) {
    return false;
  }

  // Check that the block is not a merge block.
  if (ir_context->GetStructuredCFGAnalysis()->IsMergeBlock(block->id())) {
    return false;
  }

  // Check all the fresh ids.
  std::set<uint32_t> fresh_ids_used;
  for (uint32_t id : {message_.syn_id(), message_.loop_id(), message_.ctr_id(),
                      message_.temp_id(), message_.eventual_syn_id(),
                      message_.incremented_ctr_id(), message_.cond_id()}) {
    CheckIdIsFreshAndNotUsedByThisTransformation(id, ir_context,
                                                 &fresh_ids_used);
  }

  // Only check the additional block id if it is non-zero.
  if (message_.additional_block_id()) {
    CheckIdIsFreshAndNotUsedByThisTransformation(message_.additional_block_id(),
                                                 ir_context, &fresh_ids_used);
  }

  return true;
}

void TransformationAddLoopToCreateIntConstantSynonym::Apply(
    opt::IRContext* /* ir_context */,
    TransformationContext* /* transformation_context */) const {}

protobufs::Transformation
TransformationAddLoopToCreateIntConstantSynonym::ToMessage() const {
  protobufs::Transformation result;
  *result.mutable_add_loop_to_create_int_constant_synonym() = message_;
  return result;
}

}  // namespace fuzz
}  // namespace spvtools
