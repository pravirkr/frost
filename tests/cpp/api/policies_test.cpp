#include <utility>

#include <catch2/catch_test_macros.hpp>

#include <frost/common/policies.hpp>

TEST_CASE("DataType bit widths round-trip through from_nbits/to_nbits",
          "[policies]") {
    using frost::DataType;

    CHECK(frost::bits_of(DataType::kUInt1) == 1);
    CHECK(frost::bits_of(DataType::kUInt2) == 2);
    CHECK(frost::bits_of(DataType::kUInt4) == 4);
    CHECK(frost::bits_of(DataType::kUInt8) == 8);
    CHECK(frost::bits_of(DataType::kUInt16) == 16);
    CHECK(frost::bits_of(DataType::kUInt32) == 32);
    CHECK(frost::bits_of(DataType::kFloat32) == 32);

    CHECK_FALSE(frost::is_floating(DataType::kUInt32));
    CHECK(frost::is_floating(DataType::kFloat32));

    for (const int nbits : {1, 2, 4, 8, 16, 32}) {
        const auto dtype = frost::from_nbits(nbits);
        CHECK(frost::bits_of(dtype) == nbits);
        CHECK(frost::to_nbits(dtype) == nbits);
        CHECK_FALSE(frost::is_floating(dtype));
    }

    const auto float_dtype = frost::from_nbits(-32);
    CHECK(float_dtype == DataType::kFloat32);
    CHECK(frost::to_nbits(float_dtype) == -32);
    CHECK(frost::is_floating(float_dtype));
}

TEST_CASE("from_nbits rejects unsupported bit widths", "[policies]") {
    CHECK_THROWS_AS(frost::from_nbits(3), std::invalid_argument);
    CHECK_THROWS_AS(frost::from_nbits(64), std::invalid_argument);
}

TEST_CASE("to_data_type maps psrio sample types", "[policies]") {
    using frost::DataType;

    const std::pair<psrio::SampleType, DataType> cases[] = {
        {psrio::SampleType::kUInt1, DataType::kUInt1},
        {psrio::SampleType::kUInt2, DataType::kUInt2},
        {psrio::SampleType::kUInt4, DataType::kUInt4},
        {psrio::SampleType::kUInt8, DataType::kUInt8},
        {psrio::SampleType::kUInt16, DataType::kUInt16},
        {psrio::SampleType::kUInt32, DataType::kUInt32},
        {psrio::SampleType::kFloat32, DataType::kFloat32},
    };
    for (const auto& [sample, dtype] : cases) {
        CHECK(frost::to_data_type(sample) == dtype);
        CHECK(frost::to_sample_type(dtype) == sample);
    }
    CHECK_THROWS_AS(frost::to_data_type(psrio::SampleType::kInt8),
                    std::invalid_argument);
    CHECK_THROWS_AS(frost::to_data_type(psrio::SampleType::kInt16),
                    std::invalid_argument);
}
