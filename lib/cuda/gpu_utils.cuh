#pragma once

// Included only from device translation units (lib/cuda/*.cu). Host .cpp
// files and public headers stay free of the CUDA/HIP toolkits.

#include <stdexcept>
#include <string>

#include <thrust/device_vector.h>

#include "lib/cuda/gpu_compat.cuh"

#define FROST_CUDA_CHECK(expr)                                                 \
    do {                                                                       \
        const cudaError_t frost_cuda_status = (expr);                          \
        if (frost_cuda_status != cudaSuccess) {                                \
            throw std::runtime_error(std::string("GPU error: ") +              \
                                     cudaGetErrorString(frost_cuda_status));   \
        }                                                                      \
    } while (0)

namespace frost::gpu {

template <typename T> using DeviceVector = thrust::device_vector<T>;

} // namespace frost::gpu
