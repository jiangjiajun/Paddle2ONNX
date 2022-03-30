// Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
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

#include "paddle2onnx/mapper/tensor/greater_equal.h"

namespace paddle2onnx {
REGISTER_MAPPER(greater_equal, GreaterEqualMapper)

void GreaterEqualMapper::Opset7(OnnxHelper* helper) {
  auto x_info = GetInput("X");
  auto y_info = GetInput("Y");
  auto out_info = GetOutput("Out");

  int out_dtype = 0;
  std::vector<std::string> aligned_inputs =
      helper->DtypeAlignment({x_info[0], y_info[0]}, &out_dtype);
  if (out_dtype != P2ODataType::FP32 && out_dtype != P2ODataType::FP64 &&
      helper->GetOpsetVersion() < 11) {
    aligned_inputs[0] =
        helper->AutoCast(aligned_inputs[0], out_dtype, P2ODataType::FP32);
    aligned_inputs[1] =
        helper->AutoCast(aligned_inputs[1], out_dtype, P2ODataType::FP32);
  }

  auto out = helper->MakeNode("Less", aligned_inputs)->output(0);
  helper->MakeNode("Not", {out}, {out_info[0].name});
}

void GreaterEqualMapper::Opset12(OnnxHelper* helper) {
  auto x_info = GetInput("X");
  auto y_info = GetInput("Y");
  auto out_info = GetOutput("Out");

  int out_dtype = 0;
  std::vector<std::string> aligned_inputs =
      helper->DtypeAlignment({x_info[0], y_info[0]}, &out_dtype);
  if (out_dtype != P2ODataType::FP32 && out_dtype != P2ODataType::FP64 &&
      helper->GetOpsetVersion() < 11) {
    aligned_inputs[0] =
        helper->AutoCast(aligned_inputs[0], out_dtype, P2ODataType::FP32);
    aligned_inputs[1] =
        helper->AutoCast(aligned_inputs[1], out_dtype, P2ODataType::FP32);
  }

  helper->MakeNode("GreaterOrEqual", aligned_inputs, {out_info[0].name});
}
}  // namespace paddle2onnx
