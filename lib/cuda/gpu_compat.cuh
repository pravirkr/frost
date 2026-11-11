#pragma once

// One GPU source for CUDA and HIP. Every CUDA-dialect file (lib/cuda/*.cu,
// lib/cuda/*.cuh, and the GPU internal tests) includes this header instead of
// <cuda_runtime.h> and <cuda/std/...>, and keeps using the CUDA names. In a
// HIP build (FROST_ENABLE_HIP, compiled by hip-clang) the names below are
// mapped onto HIP and rocThrust; no CUDA header is ever included there, so the
// macros cannot collide with the real CUDA API.
//
// Only the names this library uses are mapped: add a line when a new one is
// needed. Kernel-side syntax (__global__, <<<>>>, threadIdx, __syncthreads,
// __restrict__) is the same in both languages.

#if defined(FROST_ENABLE_HIP)

#include <hip/hip_runtime.h>
#include <thrust/system/hip/execution_policy.h>

// libcu++ (cuda::std) is provided by libhipcxx when it is installed; frost
// only needs span from it, so fall back to std::span otherwise. hip-clang
// compiles constexpr functions (all of std::span) for the device as well.
#if __has_include(<cuda/std/span>)
#include <cuda/std/span>
#else
#include <span>
namespace cuda::std {
using ::std::span;
} // namespace cuda::std
#endif

// rocThrust spells the CUDA system `thrust::hip`.
namespace thrust {
namespace cuda = ::thrust::hip;
} // namespace thrust

#define FROST_GPU_NAME "hip"

// Runtime: types and constants
#define cudaError_t hipError_t
#define cudaSuccess hipSuccess
#define cudaErrorInvalidDevice hipErrorInvalidDevice
#define cudaStream_t hipStream_t
#define cudaEvent_t hipEvent_t
#define cudaDeviceProp hipDeviceProp_t
#define cudaMemcpyHostToDevice hipMemcpyHostToDevice
#define cudaMemcpyDeviceToHost hipMemcpyDeviceToHost
#define cudaMemcpyDeviceToDevice hipMemcpyDeviceToDevice
#define cudaEventDisableTiming hipEventDisableTiming
#define cudaHostAllocDefault hipHostMallocDefault
#define cudaDevAttrMaxSharedMemoryPerBlockOptin                                \
    hipDeviceAttributeSharedMemPerBlockOptin
#define cudaFuncAttributeMaxDynamicSharedMemorySize                            \
    hipFuncAttributeMaxDynamicSharedMemorySize

// Runtime: functions
#define cudaGetErrorString hipGetErrorString
#define cudaGetLastError hipGetLastError
#define cudaGetDeviceCount hipGetDeviceCount
#define cudaGetDevice hipGetDevice
#define cudaSetDevice hipSetDevice
#define cudaGetDeviceProperties hipGetDeviceProperties
#define cudaDeviceGetAttribute hipDeviceGetAttribute
#define cudaDeviceSynchronize hipDeviceSynchronize
#define cudaMemGetInfo hipMemGetInfo
#define cudaMalloc hipMalloc
#define cudaFree hipFree
#define cudaHostAlloc hipHostMalloc
#define cudaFreeHost hipHostFree
#define cudaMemcpy hipMemcpy
#define cudaMemcpyAsync hipMemcpyAsync
#define cudaMemset hipMemset
#define cudaMemsetAsync hipMemsetAsync
#define cudaMemcpy2DAsync hipMemcpy2DAsync
#define cudaStreamCreate hipStreamCreate
#define cudaStreamDestroy hipStreamDestroy
#define cudaStreamSynchronize hipStreamSynchronize
#define cudaStreamWaitEvent hipStreamWaitEvent
#define cudaEventCreate hipEventCreate
#define cudaEventCreateWithFlags hipEventCreateWithFlags
#define cudaEventDestroy hipEventDestroy
#define cudaEventRecord hipEventRecord
#define cudaEventSynchronize hipEventSynchronize
#define cudaEventElapsedTime hipEventElapsedTime
// hipFuncSetAttribute takes the kernel as `const void*`. Variadic: the
// kernel argument is often a template-id whose commas would split it.
namespace frost::gpu_compat {
template <typename Kernel>
inline hipError_t
func_set_attribute(Kernel kernel, hipFuncAttribute attr, int value) {
    return hipFuncSetAttribute(reinterpret_cast<const void*>(kernel), attr,
                               value);
}
} // namespace frost::gpu_compat
#define cudaFuncSetAttribute(...)                                              \
    ::frost::gpu_compat::func_set_attribute(__VA_ARGS__)

#else // CUDA

#include <cuda/std/span>
#include <cuda_runtime.h>

#define FROST_GPU_NAME "cuda"

#endif
