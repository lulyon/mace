// Copyright 2018 Xiaomi, Inc.  All rights reserved.
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

#include <fstream>

#include "mace/core/operator.h"
#include "mace/kernels/conv_pool_2d_util.h"
#include "mace/ops/ops_test_util.h"

namespace mace {
namespace ops {
namespace test {

class WinogradConvlutionTest : public OpsTestBase {};

namespace {
void TransposeFilter(const std::vector<float> &input,
                     const std::vector<index_t> &input_shape,
                     std::vector<float> *output) {
  MACE_CHECK_NOTNULL(output);
  output->resize(input.size());

  const float *input_ptr = input.data();
  for (index_t h = 0; h < input_shape[0]; ++h) {
    for (index_t w = 0; w < input_shape[1]; ++w) {
      for (index_t oc = 0; oc < input_shape[2]; ++oc) {
        for (index_t ic = 0; ic < input_shape[3]; ++ic) {
          int offset = ((oc * input_shape[3] + ic) * input_shape[0] + h) *
                           input_shape[1] +
                       w;
          (*output)[offset] = *input_ptr;
          ++input_ptr;
        }
      }
    }
  }
}

template <DeviceType D, typename T>
void WinogradConvolution(const index_t batch,
                         const index_t height,
                         const index_t width,
                         const index_t in_channels,
                         const index_t out_channels,
                         const Padding padding) {
  // srand(time(NULL));

  // Construct graph
  OpsTestNet net;
  // Add input data
  net.AddRandomInput<D, float>("Input", {batch, height, width, in_channels});
  net.AddRandomInput<D, float>("Filter", {out_channels, in_channels, 3, 3});
  net.AddRandomInput<D, T>("Bias", {out_channels});

  BufferToImage<D, T>(&net, "Input", "InputImage",
                      kernels::BufferType::IN_OUT_CHANNEL);
  BufferToImage<D, T>(&net, "Filter", "FilterImage",
                      kernels::BufferType::CONV2D_FILTER);
  BufferToImage<D, T>(&net, "Bias", "BiasImage", kernels::BufferType::ARGUMENT);
  OpDefBuilder("Conv2D", "Conv2dTest")
      .Input("InputImage")
      .Input("FilterImage")
      .Input("BiasImage")
      .Output("OutputImage")
      .AddIntsArg("strides", {1, 1})
      .AddIntArg("padding", padding)
      .AddIntsArg("dilations", {1, 1})
      .AddIntArg("T", static_cast<int>(DataTypeToEnum<T>::value))
      .Finalize(net.NewOperatorDef());

  net.RunOp(D);

  // Transfer output
  ImageToBuffer<D, float>(&net, "OutputImage", "ConvOutput",
                      kernels::BufferType::IN_OUT_CHANNEL);
  Tensor expected;
  expected.Copy(*net.GetOutput("ConvOutput"));
  auto output_shape = expected.shape();

  // Winograd convolution
  // transform filter
  BufferToImage<D, T>(&net, "Filter", "WinoFilter",
                      kernels::BufferType::WINOGRAD_FILTER);

  // transform input
  OpDefBuilder("WinogradTransform", "WinogradTransformTest")
      .Input("InputImage")
      .Output("WinoInput")
      .AddIntArg("padding", padding)
      .AddIntArg("T", static_cast<int>(DataTypeToEnum<T>::value))
      .Finalize(net.NewOperatorDef());

  // Run on opencl
  net.RunOp(D);

  // MatMul
  OpDefBuilder("MatMul", "MatMulTest")
      .Input("WinoFilter")
      .Input("WinoInput")
      .Output("WinoGemm")
      .AddIntArg("T", static_cast<int>(DataTypeToEnum<T>::value))
      .Finalize(net.NewOperatorDef());
  // Run on opencl
  net.RunOp(D);

  // Inverse transform
  OpDefBuilder("WinogradInverseTransform", "WinogradInverseTransformTest")
      .Input("WinoGemm")
      .Input("BiasImage")
      .AddIntArg("batch", batch)
      .AddIntArg("height", output_shape[1])
      .AddIntArg("width", output_shape[2])
      .Output("WinoOutputImage")
      .AddIntArg("T", static_cast<int>(DataTypeToEnum<T>::value))
      .Finalize(net.NewOperatorDef());

  // Run on opencl
  net.RunOp(D);
  net.Sync();

  ImageToBuffer<D, float>(&net, "WinoOutputImage", "WinoOutput",
                          kernels::BufferType::IN_OUT_CHANNEL);
  if (DataTypeToEnum<T>::value == DataType::DT_HALF) {
    ExpectTensorNear<float>(expected, *net.GetOutput("WinoOutput"),
                            1e-2, 1e-2);
  } else {
    ExpectTensorNear<float>(expected, *net.GetOutput("WinoOutput"),
                            1e-5, 1e-4);
  }
}
}  // namespace

TEST_F(WinogradConvlutionTest, AlignedConvolution) {
  WinogradConvolution<DeviceType::GPU, float>(1, 32, 32, 32, 16,
                                                 Padding::VALID);
  WinogradConvolution<DeviceType::GPU, float>(1, 32, 32, 32, 16,
                                                 Padding::SAME);
}

TEST_F(WinogradConvlutionTest, UnAlignedConvolution) {
  WinogradConvolution<DeviceType::GPU, float>(1, 61, 67, 31, 37,
                                                 Padding::VALID);
  WinogradConvolution<DeviceType::GPU, float>(1, 61, 67, 37, 31,
                                                 Padding::SAME);
}

TEST_F(WinogradConvlutionTest, BatchConvolution) {
  WinogradConvolution<DeviceType::GPU, float>(3, 64, 64, 32, 32,
                                                 Padding::VALID);
  WinogradConvolution<DeviceType::GPU, float>(5, 61, 67, 37, 31,
                                                 Padding::SAME);
}

namespace {
template <DeviceType D, typename T>
void WinogradConvolutionWithPad(const index_t batch,
                                const index_t height,
                                const index_t width,
                                const index_t in_channels,
                                const index_t out_channels,
                                const int padding) {
  // srand(time(NULL));

  // Construct graph
  OpsTestNet net;
  // Add input data
  net.AddRandomInput<D, float>("Input", {batch, height, width, in_channels});
  net.AddRandomInput<D, float>("Filter", {out_channels, in_channels, 3, 3});
  net.AddRandomInput<D, float>("Bias", {out_channels});

  BufferToImage<D, T>(&net, "Input", "InputImage",
                      kernels::BufferType::IN_OUT_CHANNEL);
  BufferToImage<D, T>(&net, "Filter", "FilterImage",
                      kernels::BufferType::CONV2D_FILTER);
  BufferToImage<D, T>(&net, "Bias", "BiasImage", kernels::BufferType::ARGUMENT);
  OpDefBuilder("Conv2D", "Conv2dTest")
      .Input("InputImage")
      .Input("FilterImage")
      .Input("BiasImage")
      .Output("OutputImage")
      .AddIntsArg("strides", {1, 1})
      .AddIntsArg("padding_values", {padding, padding})
      .AddIntsArg("dilations", {1, 1})
      .AddIntArg("T", static_cast<int>(DataTypeToEnum<T>::value))
      .Finalize(net.NewOperatorDef());

  net.RunOp(D);

  // Transfer output
  ImageToBuffer<D, float>(&net, "OutputImage", "ConvOutput",
                      kernels::BufferType::IN_OUT_CHANNEL);
  Tensor expected;
  expected.Copy(*net.GetOutput("ConvOutput"));
  auto output_shape = expected.shape();

  // Winograd convolution
  // transform filter
  BufferToImage<D, T>(&net, "Filter", "WinoFilter",
                      kernels::BufferType::WINOGRAD_FILTER);

  // transform input
  OpDefBuilder("WinogradTransform", "WinogradTransformTest")
      .Input("InputImage")
      .Output("WinoInput")
      .AddIntArg("T", static_cast<int>(DataTypeToEnum<T>::value))
      .AddIntsArg("padding_values", {padding, padding})
      .Finalize(net.NewOperatorDef());

  // Run on opencl
  net.RunOp(D);

  // MatMul
  OpDefBuilder("MatMul", "MatMulTest")
      .Input("WinoFilter")
      .Input("WinoInput")
      .Output("WinoGemm")
      .AddIntArg("T", static_cast<int>(DataTypeToEnum<T>::value))
      .Finalize(net.NewOperatorDef());
  // Run on opencl
  net.RunOp(D);

  // Inverse transform
  OpDefBuilder("WinogradInverseTransform", "WinogradInverseTransformTest")
      .Input("WinoGemm")
      .Input("BiasImage")
      .AddIntArg("batch", batch)
      .AddIntArg("height", output_shape[1])
      .AddIntArg("width", output_shape[2])
      .AddIntArg("T", static_cast<int>(DataTypeToEnum<T>::value))
      .Output("WinoOutputImage")
      .Finalize(net.NewOperatorDef());

  // Run on opencl
  net.RunOp(D);
  net.Sync();

  ImageToBuffer<D, float>(&net, "WinoOutputImage", "WinoOutput",
                          kernels::BufferType::IN_OUT_CHANNEL);
  if (DataTypeToEnum<T>::value == DataType::DT_HALF) {
    ExpectTensorNear<float>(expected, *net.GetOutput("WinoOutput"),
                            1e-2, 1e-2);
  } else {
    ExpectTensorNear<float>(expected, *net.GetOutput("WinoOutput"),
                            1e-5, 1e-4);
  }
}
}  // namespace

TEST_F(WinogradConvlutionTest, AlignedConvolutionWithPad) {
  WinogradConvolutionWithPad<DeviceType::GPU, float>(1, 32, 32, 32, 16,
                                                     1);
  WinogradConvolutionWithPad<DeviceType::GPU, half>(1, 32, 32, 32, 16,
                                                    2);
}

TEST_F(WinogradConvlutionTest, UnAlignedConvolutionWithPad) {
  WinogradConvolutionWithPad<DeviceType::GPU, float>(1, 61, 67, 31, 37,
                                                     1);
  WinogradConvolutionWithPad<DeviceType::GPU, half>(1, 61, 67, 37, 31,
                                                    2);
}

TEST_F(WinogradConvlutionTest, BatchConvolutionWithPad) {
  WinogradConvolutionWithPad<DeviceType::GPU, float>(3, 64, 64, 32, 32,
                                                     1);
  WinogradConvolutionWithPad<DeviceType::GPU, half>(5, 61, 67, 37, 31,
                                                    2);
}

}  // namespace test
}  // namespace ops
}  // namespace mace
