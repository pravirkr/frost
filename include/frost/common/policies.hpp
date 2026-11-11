#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <psrio/common/types.hpp>

namespace frost {

// Basic Type Definitions
using SizeType    = std::size_t;    // Common size type
using IndexType   = std::ptrdiff_t; // Common index type (for signed indexing)
using ComplexType = std::complex<float>;

template <typename T>
concept Arithmetic = std::is_arithmetic_v<T>;

template <typename T>
concept IntegralDataType = std::is_integral_v<T>;

/**
 * @enum BasebandDataOrder
 * @brief Describes the data order for different unpacking methods.
 *
 */
enum class BasebandDataOrder : uint8_t {
    kPRITF, /**< Polarisation-Real/Imag-time-frequency */
    kFTPRI, /**< Frequency-Time-Polarisation-Real/Imag */
    kRITFP, /**< Real/Imag-time-frequency-Polarisation */
};

static const std::unordered_map<std::string_view, BasebandDataOrder>
    kBasebandDataOrderMap = {
        {"FTPRI", BasebandDataOrder::kFTPRI},
        {"PRITF", BasebandDataOrder::kPRITF},
        {"RITFP", BasebandDataOrder::kRITFP},
};

/**
 * @enum DataType
 * @brief Sample storage type for a single channel/polarisation value,
 * covering every bit depth the packed formats (sigproc, GUPPI RAW, ...) and
 * the dedispersion boundary need to describe.
 */
enum class DataType : uint8_t {
    kUInt1,
    kUInt2,
    kUInt4,
    kUInt8,
    kUInt16,
    kUInt32,
    kFloat32,
};

/** Number of bits a single sample of `dtype` occupies. */
constexpr int bits_of(DataType dtype) noexcept {
    switch (dtype) {
    case DataType::kUInt1:
        return 1;
    case DataType::kUInt2:
        return 2;
    case DataType::kUInt4:
        return 4;
    case DataType::kUInt8:
        return 8;
    case DataType::kUInt16:
        return 16;
    case DataType::kUInt32:
    case DataType::kFloat32:
        return 32;
    }
    return 0;
}

constexpr bool is_floating(DataType dtype) noexcept {
    return dtype == DataType::kFloat32;
}

/**
 * Converts a sigproc-style `nbits` field to a `DataType`.
 * By convention a negative value denotes 32-bit float samples (sigproc
 * stores `nbits == -32` for float data); positive values are packed
 * unsigned-integer widths in {1,2,4,8,16,32}.
 */
constexpr DataType from_nbits(int nbits) {
    if (nbits < 0) {
        return DataType::kFloat32;
    }
    switch (nbits) {
    case 1:
        return DataType::kUInt1;
    case 2:
        return DataType::kUInt2;
    case 4:
        return DataType::kUInt4;
    case 8:
        return DataType::kUInt8;
    case 16:
        return DataType::kUInt16;
    case 32:
        return DataType::kUInt32;
    default:
        throw std::invalid_argument("Unsupported bit width for DataType");
    }
}

/** Inverse of `from_nbits`: maps back to the sigproc signed-nbits
 * convention (float -> -32). */
constexpr int to_nbits(DataType dtype) noexcept {
    return is_floating(dtype) ? -32 : bits_of(dtype);
}

/**
 * Maps a psrio intensity sample type onto the dtype the search path
 * unpacks. Signed intensity types have no `DataType`.
 */
inline DataType to_data_type(psrio::SampleType type) {
    switch (type) {
    case psrio::SampleType::kUInt1:
        return DataType::kUInt1;
    case psrio::SampleType::kUInt2:
        return DataType::kUInt2;
    case psrio::SampleType::kUInt4:
        return DataType::kUInt4;
    case psrio::SampleType::kUInt8:
        return DataType::kUInt8;
    case psrio::SampleType::kUInt16:
        return DataType::kUInt16;
    case psrio::SampleType::kUInt32:
        return DataType::kUInt32;
    case psrio::SampleType::kFloat32:
        return DataType::kFloat32;
    case psrio::SampleType::kInt8:
        throw std::invalid_argument(
            "frost: signed 8-bit intensity is not supported");
    case psrio::SampleType::kInt16:
        throw std::invalid_argument(
            "frost: signed 16-bit intensity is not supported");
    }
    throw std::invalid_argument("frost: unsupported sample type");
}

/** Inverse of `to_data_type`. */
inline psrio::SampleType to_sample_type(DataType dtype) {
    switch (dtype) {
    case DataType::kUInt1:
        return psrio::SampleType::kUInt1;
    case DataType::kUInt2:
        return psrio::SampleType::kUInt2;
    case DataType::kUInt4:
        return psrio::SampleType::kUInt4;
    case DataType::kUInt8:
        return psrio::SampleType::kUInt8;
    case DataType::kUInt16:
        return psrio::SampleType::kUInt16;
    case DataType::kUInt32:
        return psrio::SampleType::kUInt32;
    case DataType::kFloat32:
        return psrio::SampleType::kFloat32;
    }
    throw std::invalid_argument("frost: unsupported data type");
}

} // namespace frost
