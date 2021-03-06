/**
 * Copyright (c) 2017-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef GLOW_IMPORTER_COMMONOPERATORLOADER_H
#define GLOW_IMPORTER_COMMONOPERATORLOADER_H

#include "glow/Importer/ProtobufLoader.h"

#include "glow/Base/Tensor.h"
#include "glow/Graph/Graph.h"

#include "llvm/ADT/ArrayRef.h"

#include <functional>
#include <string>
#include <unordered_map>

namespace glow {

/// Contains loaders for operators, which are common to ONNX and Caffe2 formats.
/// Every loader method adds necessary nodes to property G_, which is inherited
/// from ProtobufLoader class, therefore modifying the class instance itself.
template <typename OpType, typename AttrType>
class CommonOperatorLoader : public ProtobufLoader {
protected:
  using ArgumentDictionaryTy =
      std::unordered_map<std::string, const AttrType *>;

  virtual bool getBroadcast(const ArgumentDictionaryTy &dict) { return true; }

  void addNodeAsOutput(const OpType &op, Node *R) {
    for (int i = 0, e = op.output_size(); i < e; i++) {
      nodeValueByName_[op.output(i)] = NodeValue(R, i);
    }
  }

  /// Loads RELU operator, given its protobuf representation and parsed args.
  void loadRelu(const OpType &op, ArgumentDictionaryTy &dict) {
    const std::string &opName = loadOperatorName(op);
    auto in = getNodeValueOrCreateVariableByName(op.input(0));
    auto *R = G_.createRELU(opName, in);
    addNodeAsOutput(op, R);
  }

  void loadSigmoid(const OpType &op, ArgumentDictionaryTy &dict) {
    const std::string &opName = loadOperatorName(op);
    auto in = getNodeValueOrCreateVariableByName(op.input(0));
    auto *S = G_.createSigmoid(opName, in);
    addNodeAsOutput(op, S);
  }
  
  void loadSum(const OpType &op, ArgumentDictionaryTy &dict) {
    // TODO: support variadic arguments
    assert(op.input_size() == 2 && "Only Sum of 2 inputs is supported.");
    const std::string &opName = loadOperatorName(op);
    auto in0 = getNodeValueOrCreateVariableByName(op.input(0));
    auto in1 = getNodeValueOrCreateVariableByName(op.input(1));
    auto *node = G_.createAdd(opName, in0, in1);
    addNodeAsOutput(op, node);
  }

  void loadSoftmax(const OpType &op, ArgumentDictionaryTy &dict) {
    const std::string &opName = loadOperatorName(op);

    auto softmaxExpected =
        getNodeValueOrCreateVariableByName("softmax_expected");

    auto in = getNodeValueOrCreateVariableByName(op.input(0));

    // ONNX allows shapes like <N x 10 x 1 x 1 >. Flatten the inputs to the
    // softmax function. This is similar to a bitcast operation.
    in = G_.createFlatten("flatten", in, 1);

    auto *node = G_.createSoftMax(opName, in, softmaxExpected);
    addNodeAsOutput(op, node);
  }

  void loadLRN(const OpType &op, ArgumentDictionaryTy &dict) {
    const std::string &opName = loadOperatorName(op);
    auto in = getNodeValueOrCreateVariableByName(op.input(0));

    size_t size = loadInt(dict["size"]);
    float alpha = loadFloat(dict["alpha"]);
    float beta = loadFloat(dict["beta"]);
    float k = loadFloat(dict["bias"]);

    auto *tr = G_.createTranspose(opName, in, NCHW2NHWC);

    auto *node = G_.createLocalResponseNormalization(opName, tr, size / 2,
                                                     alpha, beta, k);

    auto *N = G_.createTranspose(opName, node, NHWC2NCHW);

    // LRN in Caffe2 has a scale_ output, but I believe it's unused for
    // inference. So explicitly only set output 0.
    nodeValueByName_[op.output(0)] = NodeValue(N, 0);
  }

  void loadArithmetic(llvm::StringRef typeName, const OpType &op,
                      ArgumentDictionaryTy &dict) {
    const std::string &opName = loadOperatorName(op);
    auto in0 = getNodeValueOrCreateVariableByName(op.input(0));
    auto in1 = getNodeValueOrCreateVariableByName(op.input(1));

    bool broadcast = getBroadcast(dict);

    Node *finalIn1 = nullptr;
    if (broadcast) {
      int axis = dict.count("axis") ? loadInt(dict["axis"]) : -1;
      // In ONNX, if axis == -1 then it sets the axis so that the
      // trailing-most dimensions are aligned like this.
      if (axis == -1) {
        axis = in0->dims(0).size() - in1->dims(0).size();
      }
      finalIn1 = G_.createBroadcast(opName, in1, in0->dims(0), axis);
    } else {
      finalIn1 = in1;
    }

    Node *node = nullptr;
    if (typeName == "Mul") {
      node = G_.createMul(opName, in0, finalIn1);
    } else if (typeName == "Add") {
      node = G_.createAdd(opName, in0, finalIn1);
    } else if (typeName == "Sub") {
      node = G_.createSub(opName, in0, finalIn1);
    } else if (typeName == "Div") {
      node = G_.createDiv(opName, in0, finalIn1);
    } else {
      assert(false && "Unsupported arithmetic typeName");
    }

    addNodeAsOutput(op, node);
  }

  void loadSplit(const OpType &op, ArgumentDictionaryTy &dict) {
    const std::string &opName = loadOperatorName(op);
    auto in = getNodeValueOrCreateVariableByName(op.input(0));
    size_t axis = dict.count("axis") ? loadInt(dict["axis"]) : 0;
    std::vector<size_t> split;
    if (dict.count("split"))
      split = getShape(dict["split"]);

    std::vector<Node *> outputs;
    G_.createSplit(opName, in, op.output_size(), axis, split, outputs);

    for (int i = 0, e = op.output_size(); i < e; i++) {
      // Each output from Split is a SliceNode which only has a single output,
      // so only use 0 here as the node value result.
      nodeValueByName_[op.output(i)] = NodeValue(outputs[i], 0);
    }
  }

  void loadReshape(const OpType &op, ArgumentDictionaryTy &dict) {
    const std::string &opName = loadOperatorName(op);
    auto in = getNodeValueOrCreateVariableByName(op.input(0));

    std::vector<size_t> newDim;
    if (dict.count("shape")) {
      std::vector<int64_t> protoDims = getShape<int64_t>(dict["shape"]);

      auto oldDim = in->dims(0);
      int64_t product = 1;
      for (size_t i = 0, e = protoDims.size(); i != e; i++) {
        if (protoDims[i] == 0)
          // shape[i] == 0 means that corresponding element should remain
          // the same.
          protoDims[i] = oldDim[i];
        if (protoDims[i] != -1)
          product *= protoDims[i];
      }
      for (size_t i = 0, e = protoDims.size(); i != e; i++) {
        if (protoDims[i] == -1)
          // shape[i] == -1 means that corresponding element should be inferred
          // from all other elements, so that Tensor size remains the same.
          protoDims[i] = in->getType(0)->size() / product;
        newDim.push_back(protoDims[i]);
      }
    } else {
      Tensor *T = getTensorByName(op.input(1));
      auto TH = T->getHandle<size_t>();
      for (size_t i = 0, e = T->size(); i != e; i++) {
        newDim.push_back(TH.raw(i));
      }
    }

    auto *node = G_.createReshape(opName, in, newDim);

    // Caffe2 sometimes outputs old_shape which goes unused. We do not currently
    // support it, so explicitly only set the first output.
    nodeValueByName_[op.output(0)] = NodeValue(node, 0);
  }

  void loadTranspose(const OpType &op, ArgumentDictionaryTy &dict,
                     llvm::StringRef permArgName) {
    const std::string &opName = loadOperatorName(op);
    auto in = getNodeValueOrCreateVariableByName(op.input(0));

    // There is a difference between ONNX and Caffe2 specs for Transpose:
    // one contains permutation under name "perm", the other contains it under
    // argument name "axes". That's why the name is passed as a parameter.
    std::vector<unsigned> perm = getShape<unsigned>(dict[permArgName]);
    if (perm.empty()) {
      // Empty permutation argument means reversing axes order.
      size_t N = in->dims(0).size();
      for (int64_t i = N - 1; i >= 0; i--)
        perm.push_back(i);
    }

    auto *T = G_.createTranspose(opName, in, perm);

    addNodeAsOutput(op, T);
  }

  void loadFlatten(const OpType &op, ArgumentDictionaryTy &dict) {
    const std::string &opName = loadOperatorName(op);
    auto in = getNodeValueOrCreateVariableByName(op.input(0));
    int axis = dict.count("axis") ? loadInt(dict["axis"]) : 1;
    auto *node = G_.createFlatten(opName, in, axis);
    addNodeAsOutput(op, node);
  }

  void loadDropout(const OpType &op, ArgumentDictionaryTy &dict) {
    auto in = getNodeValueOrCreateVariableByName(op.input(0));
    // Save the identity operation. Note the second output (mask) for Dropout in
    // Caffe2 and ONNX is unused during inference, and our Dropout does not
    // currently implement it, thus we do not call addNodeAsOutput() here.
    nodeValueByName_[op.output(0)] = NodeValue(in, 0);
  }

  using ProtobufLoader::ProtobufLoader;

  /// If operator type is supported, returns true and creates new operator.
  /// Otherwise returns false.
  bool tryLoadCommonOperator(llvm::StringRef typeName, const OpType &op,
                             ArgumentDictionaryTy &dict) {
    if (typeName == "Relu") {
      loadRelu(op, dict);
      return true;
    }
    if (typeName == "Sigmoid") {
      loadSigmoid(op, dict);
      return true;
    }
    if (typeName == "Sum") {
      loadSum(op, dict);
      return true;
    }
    if (typeName == "Softmax") {
      loadSoftmax(op, dict);
      return true;
    }
    if (typeName == "LRN") {
      loadLRN(op, dict);
      return true;
    }
    if (typeName == "Mul" || typeName == "Add" || typeName == "Sub" ||
        typeName == "Div") {
      loadArithmetic(typeName, op, dict);
      return true;
    }
    if (typeName == "Split") {
      loadSplit(op, dict);
      return true;
    }
    if (typeName == "Reshape") {
      loadReshape(op, dict);
      return true;
    }
    if (typeName == "Flatten") {
      loadFlatten(op, dict);
      return true;
    }
    if (typeName == "Dropout") {
      loadDropout(op, dict);
      return true;
    }
    return false;
  }
};

} // namespace glow

#endif // GLOW_IMPORTER_COMMONOPERATORLOADER_H
