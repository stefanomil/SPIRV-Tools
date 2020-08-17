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

#ifndef SOURCE_FUZZ_TRANSFORMATION_ADD_OPPHI_SYNONYM_H_
#define SOURCE_FUZZ_TRANSFORMATION_ADD_OPPHI_SYNONYM_H_

#include "source/fuzz/transformation.h"

namespace spvtools {
namespace fuzz {
class TransformationAddOpPhiSynonym : public Transformation {
 public:
  explicit TransformationAddOpPhiSynonym(
      const protobufs::TransformationAddOpPhiSynonym& message);

  TransformationAddOpPhiSynonym(
      uint32_t block_id, const std::map<uint32_t, uint32_t>& preds_to_ids,
      uint32_t fresh_id);

  // - |message_.block_id| is the label of a block with at least one
  //   predecessor.
  // - |message_.pred_to_id| contains a mapping from each of the predecessors of
  //   the block to an id that is available at the end of the predecessor.
  // - All the ids in |message_.pred_to_id| have been recorded as synonymous and
  //   all have the same type.
  // - The ids in |message_.pred_to_id| have one of the following types: Bool,
  //   Integer, Float, Vector, Matrix, Array, RuntimeArray, Struct.
  // - |message_.fresh_id| is a fresh id.
  bool IsApplicable(
      opt::IRContext* ir_context,
      const TransformationContext& transformation_context) const override;

  // Given a block with n predecessors, with n >= 1, and n corresponding
  // synonymous ids of the same type, each available to use at the end of the
  // corresponding predecessor, adds an OpPhi instruction at the beginning of
  // the block of the form:
  //   %fresh_id = OpPhi %type %id_1 %pred_1 %id_2 %pred_2 ... %id_n %pred_n
  // This instruction is then marked as synonymous with the ids.
  void Apply(opt::IRContext* ir_context,
             TransformationContext* transformation_context) const override;

  // Returns true if |type_id| is the id of a type in the module, which is one
  // of the following: Bool, Integer, Float, Vector, Matrix, Array,
  // RuntimeArray, Struct.
  static bool CheckTypeIsAllowed(opt::IRContext* ir_context, uint32_t type_id);

  protobufs::Transformation ToMessage() const override;

 private:
  protobufs::TransformationAddOpPhiSynonym message_;
};
}  // namespace fuzz
}  // namespace spvtools

#endif  // SOURCE_FUZZ_TRANSFORMATION_ADD_OPPHI_SYNONYM_H_