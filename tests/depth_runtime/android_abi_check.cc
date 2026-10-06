// SPDX-License-Identifier: Apache-2.0
// Compile-time check of the private dynamic-loader ABI against the actual NDK.
#include <android/NeuralNetworksTypes.h>
#include "../../common/depth/experimental.cc"
#include <cstddef>
using oc::depth_test::detail::Operand;
static_assert(sizeof(Operand)==sizeof(ANeuralNetworksOperandType), "operand ABI size");
static_assert(alignof(Operand)==alignof(ANeuralNetworksOperandType), "operand ABI alignment");
static_assert(offsetof(Operand, dimensions)==offsetof(ANeuralNetworksOperandType, dimensions), "dimensions ABI");
static_assert(offsetof(Operand, zeroPoint)==offsetof(ANeuralNetworksOperandType, zeroPoint), "zero ABI");
static_assert(ANEURALNETWORKS_INT32==1 && ANEURALNETWORKS_TENSOR_INT32==4 &&
              ANEURALNETWORKS_TENSOR_QUANT8_ASYMM==5 && ANEURALNETWORKS_CONV_2D==3 &&
              ANEURALNETWORKS_PADDING_SAME==1 && ANEURALNETWORKS_FUSED_NONE==0 &&
              ANEURALNETWORKS_FUSED_RELU==1 && ANEURALNETWORKS_PREFER_FAST_SINGLE_ANSWER==1 &&
              ANEURALNETWORKS_DEVICE_ACCELERATOR==4 &&
              ANEURALNETWORKS_MISSED_DEADLINE_TRANSIENT==10 &&
              ANEURALNETWORKS_MISSED_DEADLINE_PERSISTENT==11, "NNAPI enum ABI");
