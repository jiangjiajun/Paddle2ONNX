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

#include "paddle2onnx/mapper/exporter.h"
#include <onnx/checker.h>
#include "onnxoptimizer/optimize.h"
#include "paddle2onnx/optimizer/eliminate_non_transpose.h"
#include "paddle2onnx/optimizer/fuse_constant_cast.h"
#include "paddle2onnx/optimizer/fuse_constant_reshape.h"
#include "paddle2onnx/optimizer/fuse_constant_unsqueeze.h"
#include "paddle2onnx/optimizer/fuse_paddle_conv_bias.h"
#include "paddle2onnx/optimizer/fuse_unsqueeze_conv2d_squeeze.h"

namespace paddle2onnx {
MapperHelper* MapperHelper::helper = nullptr;

void ModelExporter::ExportParameters(
    const std::map<std::string, Weight>& params, bool use_initializer) {
  for (auto& item : params) {
    // TODO(jiangjiajun) I'm not handling use_initializer now, but some day I
    // will
    auto node = MakeConstant(item.first, item.second);
    parameters.push_back(std::move(node));
  }
}

void ModelExporter::ExportInputOutputs(
    const std::vector<TensorInfo>& input_infos,
    const std::vector<TensorInfo>& output_infos) {
  for (auto& item : input_infos) {
    auto value_info = MakeValueInfo(item);
    inputs.push_back(std::move(value_info));
  }
  for (auto& item : output_infos) {
    auto value_info = MakeValueInfo(item);
    outputs.push_back(std::move(value_info));
  }
}

void ModelExporter::ExportOp(const PaddleParser& parser, OnnxHelper* helper,
                             int32_t opset_version, int64_t block_id,
                             int64_t op_id, bool verbose) {
  auto op = parser.GetOpDesc(block_id, op_id);
  if (verbose) {
    std::cerr << "Converting operator:" << op.type() << std::endl;
  }
  if (op.type() == "while") {
    return ExportLoop(parser, helper, opset_version, block_id, op_id, verbose);
  }
  auto mapper =
      MapperHelper::Get()->CreateMapper(op.type(), parser, block_id, op_id);
  mapper->Run(helper, opset_version);
  delete mapper;
}

void ModelExporter::ProcessGraphDumplicateNames(
    std::vector<std::shared_ptr<ONNX_NAMESPACE::NodeProto>>* parameters,
    std::vector<std::shared_ptr<ONNX_NAMESPACE::ValueInfoProto>>* inputs,
    std::vector<std::shared_ptr<ONNX_NAMESPACE::ValueInfoProto>>* outputs,
    std::vector<std::shared_ptr<ONNX_NAMESPACE::NodeProto>>* nodes) {
  // process dumplicate tensor names
  std::unordered_map<std::string, std::string> renamer;
  std::set<std::string> tensor_names;
  for (auto& item : *parameters) {
    for (size_t i = 0; i < item->output_size(); ++i) {
      if (tensor_names.find(item->output(i)) != tensor_names.end()) {
        Assert(false, "There's dumplicate names in exported parameters.");
      }
      tensor_names.insert(item->output(i));
    }
  }
  for (auto& item : *inputs) {
    if (tensor_names.find(item->name()) != tensor_names.end()) {
      Assert(false,
             "There's dumplicate names in exported parameters and inputs.");
    }
    tensor_names.insert(item->name());
  }
  for (auto& item : *nodes) {
    // update node inputs
    for (size_t i = 0; i < item->input_size(); ++i) {
      if (renamer.find(item->input(i)) != renamer.end()) {
        auto updated_name = renamer[item->input(i)];
        while (renamer.find(updated_name) != renamer.end()) {
          updated_name = renamer[updated_name];
        }
        *(item->mutable_input(i)) = updated_name;
      }
    }
    // if there's dumplicate name
    // will generate new name and replace it
    for (size_t i = 0; i < item->output_size(); ++i) {
      if (tensor_names.find(item->output(i)) != tensor_names.end()) {
        std::string renamed_tensor_name = item->output(i);
        while (renamer.find(renamed_tensor_name) != renamer.end()) {
          renamed_tensor_name = renamer[renamed_tensor_name];
        }
        auto new_tensor_name =
            MapperHelper::Get()->GenName(renamed_tensor_name);
        std::cerr << "[Renamer] Find dumplicate output name: "
                  << renamed_tensor_name << " in model." << std::endl;
        std::cerr << "[Renamer] Will rename " << renamed_tensor_name << " to "
                  << new_tensor_name << std::endl;
        *(item->mutable_output(i)) = new_tensor_name;
        renamer[renamed_tensor_name] = new_tensor_name;
      }
      tensor_names.insert(item->output(i));
    }
  }

  for (auto& item : *outputs) {
    if (renamer.find(item->name()) != renamer.end()) {
      auto updated_name = renamer[item->name()];
      while (renamer.find(updated_name) != renamer.end()) {
        updated_name = renamer[updated_name];
      }
      item->set_name(updated_name);
    }
  }
}

std::string ModelExporter::Run(const PaddleParser& parser, int opset_version,
                               bool auto_upgrade_opset, bool verbose,
                               bool enable_onnx_checker,
                               bool enable_experimental_op,
                               bool enable_optimize) {
  Assert(opset_version <= 15 && opset_version >= 7,
         "Paddle2ONNX now only support opset version in range of [7, 15].");
  _helper.Clear();
  inputs.clear();
  outputs.clear();
  parameters.clear();

  // clear name_counter
  // this use to generate unique name
  // for intermdiate
  // while converting all the op
  MapperHelper::Get()->ClearNameCounter();

  std::set<std::string> unsupported_ops;
  if (!CheckIfOpSupported(parser, &unsupported_ops, enable_experimental_op)) {
    std::cerr << "Oops, there are some operators not supported by Paddle2ONNX "
                 "yet, list as below "
              << std::endl;
    for (auto& item : unsupported_ops) {
      std::cerr << "=====: " << item << std::endl;
    }
    Assert(1 == 0,
           "Due to the unsupported operators, the conversion is aborted.");
  }

  int32_t min_opset = GetMinOpset(parser, opset_version, false);
  if (min_opset < 0) {
    min_opset = GetMinOpset(parser, opset_version, true);
    Assert(false,
           "Model exporting failed, you can report this problem to "
           "https://github.com/PaddlePaddle/Paddle2ONNX.git.");
  }
  if (!auto_upgrade_opset) {
    if (min_opset > opset_version) {
      min_opset = GetMinOpset(parser, opset_version, true);
      std::cerr << "This model cannot export to onnx with opset version = "
                << opset_version
                << ", the opset version for this model should be greater or "
                   "equal than "
                << min_opset << std::endl;
      Assert(false,
             "Due to opset version, the model exporting is aborted, please set "
             "a higher opset_version or set auto_upgrade_opset=true.");
    }
  } else {
    if (min_opset > opset_version) {
      std::cerr << "Opset version has been changed to " << min_opset << " from "
                << opset_version << std::endl;
      opset_version = min_opset;
    }
  }
  _helper.SetOpsetVersion(opset_version);
  std::cerr << "Model will exported with opset = " << _helper.GetOpsetVersion()
            << std::endl;

  ExportParameters(parser.params);
  ExportInputOutputs(parser.inputs, parser.outputs);

  // Only convert blocks 0 now
  // because control flow is not supported yet
  for (auto i = 0; i < parser.NumOfOps(0); ++i) {
    auto op = parser.GetOpDesc(0, i);
    if (op.type() == "feed") {
      continue;
    } else if (op.type() == "fetch") {
      continue;
    }
    ExportOp(parser, &_helper, opset_version, 0, i, verbose);
  }

  // construct a onnx model proto
  auto model = std::make_shared<ONNX_NAMESPACE::ModelProto>();
  // TODO(jiangjiajun) ir version is related to onnx version
  model->set_ir_version(ONNX_NAMESPACE::IR_VERSION);
  auto graph = model->mutable_graph();
  graph->set_name("Model from PaddlePaddle.");
  auto opset_id = model->add_opset_import();
  // TODO(jiangjiajun) custom op is not considered
  opset_id->set_domain("");
  opset_id->set_version(opset_version);

  ProcessGraphDumplicateNames(&parameters, &inputs, &outputs, &_helper.nodes);

  for (auto& item : parameters) {
    *(graph->add_node()) = *(item.get());
  }
  for (auto& item : inputs) {
    *(graph->add_input()) = *(item.get());
  }
  for (auto& item : _helper.nodes) {
    *(graph->add_node()) = (*item.get());
  }
  for (auto& item : outputs) {
    *(graph->add_output()) = (*item.get());
  }
  for (auto& item : _helper.value_infos) {
    *(graph->add_value_info()) = (*item.get());
  }

  // TODO(jiangjiajun)
  // If we need to integrate with framework
  // this check will return a information
  // to let framework know the conversion is
  // pass or fail
  if (enable_onnx_checker) {
    try {
      ONNX_NAMESPACE::checker::check_model(*(model.get()));
    } catch (...) {
      std::cerr << "[Paddle2ONNX] ONNX model conversion is invalid."
                << std::endl;
      return "";
    }
    std::cerr << "[Paddle2ONNX] ONNX model conversion is valid." << std::endl;
  }

  std::string out;
  if (enable_optimize) {
    auto const opt_model = Optimize(*(model.get()));
    if (!opt_model.SerializeToString(&out)) {
      if (verbose) {
        std::cerr << "ONNX Model SerializeToString error" << std::endl;
      }
      return "";
    }
  } else {
    if (!model->SerializeToString(&out)) {
      if (verbose) {
        std::cerr << "ONNX Model SerializeToString error" << std::endl;
      }
      return "";
    }
  }
  return out;
}

bool ModelExporter::CheckIfOpSupported(const PaddleParser& parser,
                                       std::set<std::string>* unsupported_ops,
                                       bool enable_experimental_op) {
  unsupported_ops->clear();
  for (auto i = 0; i < parser.NumOfBlocks(); ++i) {
    for (auto j = 0; j < parser.NumOfOps(i); ++j) {
      auto op = parser.GetOpDesc(i, j);
      if (op.type() == "feed" || op.type() == "fetch") {
        continue;
      }
      if (op.type() == "while" && enable_experimental_op) {
        if (!IsLoopSupported(parser, i, j)) {
          unsupported_ops->insert("while");
        }
        continue;
      }
      if (!MapperHelper::Get()->IsRegistered(op.type())) {
        unsupported_ops->insert(op.type());
      } else if (!enable_experimental_op) {
        auto mapper =
            MapperHelper::Get()->CreateMapper(op.type(), parser, i, j);
        if (mapper->IsExperimentalOp()) {
          unsupported_ops->insert(op.type());
        }
        delete mapper;
      }
    }
  }
  return (unsupported_ops->size() == 0);
}

int32_t ModelExporter::GetMinOpset(const PaddleParser& parser,
                                   const int32_t& opset_version, bool verbose) {
  int32_t max_opset = -1;
  bool exportable = true;
  // Record the number of ops that need to be converted
  int converted_op_num = 0;
  std::set<std::string> verbose_log;
  for (auto i = 0; i < parser.NumOfBlocks(); ++i) {
    for (auto j = 0; j < parser.NumOfOps(i); ++j) {
      auto op = parser.GetOpDesc(i, j);
      if (op.type() == "feed" || op.type() == "fetch") {
        continue;
      }
      converted_op_num += 1;
      int current_min_opset = 7;
      if (op.type() == "while") {
        current_min_opset = 13;
      } else {
        auto mapper =
            MapperHelper::Get()->CreateMapper(op.type(), parser, i, j);
        current_min_opset = mapper->GetMinOpset(verbose);
        delete mapper;
      }
      if (current_min_opset < 0) {
        exportable = false;
        if (verbose) {
          std::cerr << "[Paddle2ONNX] Due to the operator: " << op.type()
                    << ", this model cannot be exported." << std::endl;
        }
      } else if (current_min_opset > max_opset) {
        max_opset = current_min_opset;
        if (verbose && current_min_opset > opset_version) {
          verbose_log.insert("[Paddle2ONNX] Due to the operator: " + op.type() +
                             ", requires opset_version >= " +
                             std::to_string(current_min_opset) + ".");
        }
      }
    }
  }
  if (verbose) {
    for (auto iter = verbose_log.begin(); iter != verbose_log.end(); ++iter) {
      std::cerr << *iter << std::endl;
    }
  }
  // If there are only feed and fetch op in Paddle model
  if (!converted_op_num) {
    return 7;
  }

  // Here we put some checks to make sure
  // paddle2onnx could compatible with
  // other version of onnx
  int32_t max_support_opset = MAX_ONNX_OPSET_VERSION;
  if (exportable && (max_opset > MAX_ONNX_OPSET_VERSION)) {
    exportable = false;
    if (verbose) {
      std::cerr << "[ERROR] The compiled onnx version only support opset 7~"
                << MAX_ONNX_OPSET_VERSION
                << ", but now this model need at least opset " << max_opset
                << ", please compile with higher version of onnx." << std::endl;
    }
  }
  if (exportable) {
    return max_opset;
  }

  return -1;
}

ONNX_NAMESPACE::ModelProto ModelExporter::Optimize(
    const ONNX_NAMESPACE::ModelProto& model) {
  ONNX_NAMESPACE::optimization::Optimizer::passes
      .registerPass<ONNX_NAMESPACE::optimization::FuseConstantReshape>();
  ONNX_NAMESPACE::optimization::Optimizer::passes
      .registerPass<ONNX_NAMESPACE::optimization::FuseConstantUnsqueeze>();
  ONNX_NAMESPACE::optimization::Optimizer::passes
      .registerPass<ONNX_NAMESPACE::optimization::FusePaddleConvBias>();
  ONNX_NAMESPACE::optimization::Optimizer::passes
      .registerPass<ONNX_NAMESPACE::optimization::FuseUnsqueezeConv2dSqueeze>();
  ONNX_NAMESPACE::optimization::Optimizer::passes
      .registerPass<ONNX_NAMESPACE::optimization::EliminateNonTranspose>();
  ONNX_NAMESPACE::optimization::Optimizer::passes
      .registerPass<ONNX_NAMESPACE::optimization::FuseConstantCast>();
  std::vector<std::string> passes = {
      "eliminate_identity", "eliminate_deadend", "eliminate_deadend",
      "fuse_constant_reshape", "fuse_constant_unsqueeze",
      //                                     "fuse_constant_cast",
      "fuse_paddle_conv_bias", //"fuse_unsqueeze_conv2d_squeeze",
      "fuse_consecutive_transposes", "eliminate_non_transpose",
      "fuse_matmul_add_bias_into_gemm", "eliminate_identity",
      "eliminate_deadend", "eliminate_unused_initializer"};
  return ONNX_NAMESPACE::optimization::Optimize(model, passes);
}

}  // namespace paddle2onnx
