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

#ifndef SOURCE_FUZZ_FUZZER_PASS_ADD_LOOP_PREHEADERS_H
#define SOURCE_FUZZ_FUZZER_PASS_ADD_LOOP_PREHEADERS_H

#include "source/fuzz/fuzzer_pass.h"

namespace spvtools {
namespace fuzz {

// A fuzzer pass that randomly adds loop preheaders to headers that do not have
// one that satisfies all of the following conditions:
// - it is the only out-of-loop predecessor of the header
// - it branches unconditionally to the header
// - it is not a loop header itself
class FuzzerPassAddLoopPreheaders : public FuzzerPass {
 public:
  FuzzerPassAddLoopPreheaders(
      opt::IRContext* ir_context, TransformationContext* transformation_context,
      FuzzerContext* fuzzer_context,
      protobufs::TransformationSequence* transformations);

  ~FuzzerPassAddLoopPreheaders();

  void Apply() override;
};

}  // namespace fuzz
}  // namespace spvtools

#endif  // SOURCE_FUZZ_FUZZER_PASS_ADD_LOOP_PREHEADERS_H
