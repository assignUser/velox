/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "velox/exec/prefixsort/PrefixSortEncoder.h"
#include "velox/vector/fuzzer/VectorFuzzer.h"
#include "velox/vector/tests/VectorTestUtils.h"

namespace facebook::velox::exec::prefixsort::test {

namespace {
// Since 'std::numeric_limit<Timestamp>' has not yet implemented: is_integer,
// is_signed, quiet_NaN(), add TypeLimits struct to skip this.
template <typename T>
struct TypeLimits {
  static const bool isFloat = !std::numeric_limits<T>::is_integer;
  static const bool isSigned = std::numeric_limits<T>::is_signed;

  static FOLLY_ALWAYS_INLINE T min() {
    // Since std::numeric_limits<T>::min() returns 'the minimum finite value, or
    // for floating types with denormalization, the minimum positive normalized
    // value', we use -max as min in float types.
    return std::numeric_limits<T>::is_integer ? std::numeric_limits<T>::min()
                                              : -std::numeric_limits<T>::max();
  }

  // For signed numbers mid is 0.
  // For unsigned numbers mid is max / 2.
  static FOLLY_ALWAYS_INLINE T mid() {
    return isSigned ? (T)0 : std::numeric_limits<T>::max() / 2;
  }

  static FOLLY_ALWAYS_INLINE T nan() {
    return std::numeric_limits<T>::quiet_NaN();
  }
};

template <>
struct TypeLimits<Timestamp> {
  static const bool isFloat = false;
  static FOLLY_ALWAYS_INLINE Timestamp min() {
    return std::numeric_limits<Timestamp>::min();
  }

  static FOLLY_ALWAYS_INLINE Timestamp mid() {
    return Timestamp();
  }

  // Never be called, just for skipping compile error.
  static FOLLY_ALWAYS_INLINE Timestamp nan() {
    VELOX_UNSUPPORTED("Timestamp not support nan()");
  }
};
} // namespace

class PrefixEncoderTest : public testing::Test,
                          public velox::test::VectorTestBase {
 public:
  template <typename T>
  void testEncodeNoNull(T value, char* expectedAsc, char* expectedDesc) {
    char encoded[sizeof(T)];
    ascNullsFirstEncoder_.encodeNoNulls(value, (char*)encoded, sizeof(T) + 1);
    ASSERT_EQ(std::memcmp(encoded, expectedAsc, sizeof(T)), 0);
    descNullsFirstEncoder_.encodeNoNulls(value, (char*)encoded, sizeof(T) + 1);
    ASSERT_EQ(std::memcmp(encoded, expectedDesc, sizeof(T)), 0);
  }

  template <typename T>
  void testEncodeWithNull(T testValue, char* expectedAsc, char* expectedDesc) {
    std::optional<T> nullValue = std::nullopt;
    std::optional<T> value = testValue;
    char encoded[sizeof(T) + 1];
    char nullFirst[sizeof(T) + 1];
    char nullLast[sizeof(T) + 1];
    memset(nullFirst, 0, sizeof(T) + 1);
    memset(nullLast, 1, 1);
    memset(nullLast + 1, 0, sizeof(T));

    auto compare = [](char* left, char* right) {
      return std::memcmp(left, right, sizeof(T) + 1);
    };

    ascNullsFirstEncoder_.encode(nullValue, encoded, sizeof(T) + 1, true);
    ASSERT_EQ(compare(nullFirst, encoded), 0);
    ascNullsLastEncoder_.encode(nullValue, encoded, sizeof(T) + 1, true);
    ASSERT_EQ(compare(nullLast, encoded), 0);

    ascNullsFirstEncoder_.encode(value, encoded, sizeof(T) + 1, true);
    ASSERT_EQ(encoded[0], 1);
    ASSERT_EQ(std::memcmp(encoded + 1, expectedAsc, sizeof(T)), 0);
    ascNullsLastEncoder_.encode(value, encoded, sizeof(T) + 1, true);
    ASSERT_EQ(encoded[0], 0);
    ASSERT_EQ(std::memcmp(encoded + 1, expectedAsc, sizeof(T)), 0);
    descNullsFirstEncoder_.encode(value, encoded, sizeof(T) + 1, true);
    ASSERT_EQ(encoded[0], 1);
    ASSERT_EQ(std::memcmp(encoded + 1, expectedDesc, sizeof(T)), 0);
    descNullsLastEncoder_.encode(value, encoded, sizeof(T) + 1, true);
    ASSERT_EQ(encoded[0], 0);
    ASSERT_EQ(std::memcmp(encoded + 1, expectedDesc, sizeof(T)), 0);
  }

  template <typename T>
  void testEncode(T value, char* expectedAsc, char* expectedDesc) {
    testEncodeNoNull<T>(value, expectedAsc, expectedDesc);
    testEncodeWithNull<T>(value, expectedAsc, expectedDesc);
  }

  template <typename T>
  void testEncodeWithColumnNoNulls(
      std::optional<T> value,
      char* expectedAsc,
      char* expectedDesc) {
    char encoded[sizeof(T)];
    ascNullsFirstEncoder_.encode(value, encoded, sizeof(T), false);
    ASSERT_EQ(std::memcmp(encoded, expectedAsc, sizeof(T)), 0);
    ascNullsLastEncoder_.encode(value, encoded, sizeof(T), false);
    ASSERT_EQ(std::memcmp(encoded, expectedAsc, sizeof(T)), 0);
    descNullsFirstEncoder_.encode(value, encoded, sizeof(T), false);
    ASSERT_EQ(std::memcmp(encoded, expectedDesc, sizeof(T)), 0);
    descNullsLastEncoder_.encode(value, encoded, sizeof(T), false);
    ASSERT_EQ(std::memcmp(encoded, expectedDesc, sizeof(T)), 0);
  }

  template <typename T>
  void testNullCompare() {
    constexpr uint32_t kEncodeSize = sizeof(T) + 1;
    std::optional<T> nullValue = std::nullopt;
    std::optional<T> max = std::numeric_limits<T>::max();
    std::optional<T> min = std::numeric_limits<T>::min();
    char encodedNull[kEncodeSize];
    char encodedMax[kEncodeSize];
    char encodedMin[kEncodeSize];

    auto encode = [&](auto& encoder) {
      encoder.encode(nullValue, encodedNull, kEncodeSize, true);
      encoder.encode(min, encodedMin, kEncodeSize, true);
      encoder.encode(max, encodedMax, kEncodeSize, true);
    };

    auto compare = [](char* left, char* right) {
      return std::memcmp(left, right, kEncodeSize);
    };

    // Nulls first: NULL < non-NULL.
    encode(ascNullsFirstEncoder_);
    ASSERT_LT(compare(encodedNull, encodedMin), 0);
    encode(descNullsFirstEncoder_);
    ASSERT_LT(compare(encodedNull, encodedMin), 0);

    // Nulls last: NULL > non-NULL.
    encode(ascNullsLastEncoder_);
    ASSERT_GT(compare(encodedNull, encodedMax), 0);
    encode(descNullsLastEncoder_);
    ASSERT_GT(compare(encodedNull, encodedMax), 0);

    // For float / double`s NaN.
    if (TypeLimits<T>::isFloat) {
      std::optional<T> nan = TypeLimits<T>::nan();
      char encodedNaN[kEncodeSize];

      ascNullsFirstEncoder_.encode(nan, encodedNaN, kEncodeSize, true);
      ascNullsFirstEncoder_.encode(max, encodedMax, kEncodeSize, true);
      ASSERT_GT(compare(encodedNaN, encodedMax), 0);

      ascNullsFirstEncoder_.encode(nan, encodedNaN, kEncodeSize, true);
      ascNullsFirstEncoder_.encode(nullValue, encodedNull, kEncodeSize, true);
      ASSERT_LT(compare(encodedNull, encodedNaN), 0);
    }
  }

  template <typename T>
  void testValidValueCompare() {
    constexpr uint32_t kEncodeSize = sizeof(T) + 1;
    std::optional<T> max = std::numeric_limits<T>::max();
    std::optional<T> min = TypeLimits<T>::min();
    std::optional<T> mid = TypeLimits<T>::mid();
    char encodedMax[kEncodeSize];
    char encodedMin[kEncodeSize];
    char encodedMid[kEncodeSize];
    auto encode = [&](auto& encoder) {
      encoder.encode(mid, encodedMid, kEncodeSize, true);
      encoder.encode(min, encodedMin, kEncodeSize, true);
      encoder.encode(max, encodedMax, kEncodeSize, true);
    };

    auto compare = [](char* left, char* right) {
      return std::memcmp(left, right, kEncodeSize);
    };

    encode(ascNullsFirstEncoder_);
    // ASC: min < mid < max.
    ASSERT_GT(compare(encodedMid, encodedMin), 0);
    ASSERT_LT(compare(encodedMid, encodedMax), 0);

    encode(descNullsFirstEncoder_);
    // DESC: max < mid < min.
    ASSERT_LT(compare(encodedMid, encodedMin), 0);
    ASSERT_GT(compare(encodedMid, encodedMax), 0);

    encode(ascNullsLastEncoder_);
    // ASC: min < mid < max.
    ASSERT_GT(compare(encodedMid, encodedMin), 0);
    ASSERT_LT(compare(encodedMid, encodedMax), 0);

    encode(descNullsLastEncoder_);
    // DESC: max < mid < min.
    ASSERT_LT(compare(encodedMid, encodedMin), 0);
    ASSERT_GT(compare(encodedMid, encodedMax), 0);
  }

  template <typename T>
  void testCompare() {
    testNullCompare<T>();
    testValidValueCompare<T>();
  }

  template <TypeKind Kind>
  void testFuzz() {
    using ValueDataType = typename TypeTraits<Kind>::NativeType;
    const int vectorSize = 1024;

    auto compare = [](char* left, char* right) {
      const auto result = std::memcmp(left, right, sizeof(ValueDataType) + 1);
      // Keeping the result of memory compare consistent with the result of
      // Vector`s compare method can facilitate ASSERT_EQ.
      return result < 0 ? -1 : (result > 0 ? 1 : 0);
    };

    auto test = [&](const PrefixSortEncoder& encoder) {
      TypePtr type = TypeTraits<Kind>::ImplType::create();
      VectorFuzzer fuzzer(
          {.vectorSize = vectorSize, .nullRatio = 0.1, .stringLength = 16},
          pool());

      CompareFlags compareFlag = {
          encoder.isNullsFirst(),
          encoder.isAscending(),
          false,
          CompareFlags::NullHandlingMode::kNullAsValue};
      SCOPED_TRACE(compareFlag.toString());
      const auto leftVector =
          std::dynamic_pointer_cast<FlatVector<ValueDataType>>(
              fuzzer.fuzzFlat(type, vectorSize));
      const auto rightVector =
          std::dynamic_pointer_cast<FlatVector<ValueDataType>>(
              fuzzer.fuzzFlat(type, vectorSize));

      char leftEncoded[sizeof(ValueDataType) + 1];
      char rightEncoded[sizeof(ValueDataType) + 1];

      for (auto i = 0; i < vectorSize; ++i) {
        const auto leftValue = leftVector->isNullAt(i)
            ? std::nullopt
            : std::optional<ValueDataType>(leftVector->valueAt(i));
        const auto rightValue = rightVector->isNullAt(i)
            ? std::nullopt
            : std::optional<ValueDataType>(rightVector->valueAt(i));
        if constexpr (
            Kind == TypeKind::VARCHAR || Kind == TypeKind::VARBINARY) {
          encoder.encode(leftValue, leftEncoded, 17, true);
          encoder.encode(rightValue, rightEncoded, 17, true);
        } else {
          encoder.encode(
              leftValue, leftEncoded, sizeof(ValueDataType) + 1, true);
          encoder.encode(
              rightValue, rightEncoded, sizeof(ValueDataType) + 1, true);
        }

        const auto result = compare(leftEncoded, rightEncoded);
        const auto expected =
            leftVector->compare(rightVector.get(), i, i, compareFlag).value();
        ASSERT_EQ(result, expected);
      }
    };

    test(ascNullsFirstEncoder_);
    test(ascNullsLastEncoder_);
    test(descNullsFirstEncoder_);
    test(descNullsLastEncoder_);
  }

  const PrefixSortEncoder& ascNullsFirstEncoder() const {
    return ascNullsFirstEncoder_;
  }

  const PrefixSortEncoder ascNullsLastEncoder() const {
    return ascNullsLastEncoder_;
  }

  const PrefixSortEncoder descNullsFirstEncoder() const {
    return descNullsFirstEncoder_;
  }

  const PrefixSortEncoder descNullsLastEncoder() const {
    return descNullsLastEncoder_;
  }

 protected:
  static void SetUpTestCase() {
    memory::MemoryManager::testingSetInstance(memory::MemoryManager::Options{});
  }

 private:
  const PrefixSortEncoder ascNullsFirstEncoder_ = {true, true};
  const PrefixSortEncoder ascNullsLastEncoder_ = {true, false};
  const PrefixSortEncoder descNullsFirstEncoder_ = {false, true};
  const PrefixSortEncoder descNullsLastEncoder_ = {false, false};
};

TEST_F(PrefixEncoderTest, encode) {
  {
    uint64_t ascExpected = 0x8877665544332211;
    uint64_t descExpected = 0x778899aabbccddee;
    testEncode<uint64_t>(
        0x1122334455667788, (char*)&ascExpected, (char*)&descExpected);
  }

  {
    int64_t ascExpected = 0x8877665544332291;
    int64_t descExpected = 0x778899aabbccdd6e;
    testEncode<int64_t>(
        0x1122334455667788, (char*)&ascExpected, (char*)&descExpected);
  }
  {
    uint32_t ascExpected = 0x44332211;
    uint32_t descExpected = 0xbbccddee;
    testEncode<uint32_t>(0x11223344, (char*)&ascExpected, (char*)&descExpected);
  }
  {
    int32_t ascExpected = 0x44332291;
    int32_t descExpected = 0xbbccdd6e;
    testEncode<int32_t>(0x11223344, (char*)&ascExpected, (char*)&descExpected);
  }

  {
    uint16_t ascExpected = 0x2211;
    uint16_t descExpected = 0xddee;
    testEncode<uint16_t>(0x1122, (char*)&ascExpected, (char*)&descExpected);
  }
  {
    int16_t ascExpected = 0x2291;
    int16_t descExpected = 0xdd6e;
    testEncode<int16_t>(0x1122, (char*)&ascExpected, (char*)&descExpected);
  }

  {
    uint32_t ascExpected = 0x0050c3c7;
    uint32_t descExpected = 0xffaf3c38;
    testEncode<float>(100000.00, (char*)&ascExpected, (char*)&descExpected);
  }

  {
    uint64_t ascExpected = 0x00000000006af8c0;
    uint64_t descExpected = 0xffffffffff95073f;
    testEncode<double>(100000.00, (char*)&ascExpected, (char*)&descExpected);
  }

  {
    char ascExpected[16] = {
        -128, 0, 0, 0, 0, 0, 0, 10, 0, 0, 0, 0, 0, 0, 0, -56};
    char descExpected[16] = {
        127, -1, -1, -1, -1, -1, -1, -11, -1, -1, -1, -1, -1, -1, -1, 55};
    int128_t value = HugeInt::build(10, 200);
    testEncode<int128_t>(value, (char*)ascExpected, (char*)descExpected);
  }

  {
    Timestamp value = Timestamp(0x000000011223344, 0x000000011223344);
    uint64_t ascExpected[2];
    uint64_t descExpected[2];
    ascExpected[0] = 0x4433221100000080;
    ascExpected[1] = 0x4433221100000000;
    descExpected[0] = 0xbbccddeeffffff7f;
    descExpected[1] = 0xbbccddeeffffffff;
    testEncode<Timestamp>(value, (char*)ascExpected, (char*)descExpected);
  }
}

TEST_F(PrefixEncoderTest, encodeString) {
  constexpr uint32_t kEncodeSize = 13;
  StringView testValue = StringView("aaaaaabbbbbb");
  char expectedAsc[kEncodeSize] = "aaaaaabbbbbb";
  char expectedDesc[kEncodeSize];
  for (int i = 0; i < kEncodeSize - 1; ++i) {
    expectedDesc[i] = ~expectedAsc[i];
  }
  std::optional<StringView> nullValue = std::nullopt;
  std::optional<StringView> value = testValue;
  char encoded[kEncodeSize + 1];
  char nullFirst[kEncodeSize + 1];
  char nullLast[kEncodeSize + 1];
  memset(nullFirst, 0, kEncodeSize);
  memset(nullLast, 1, 1);
  memset(nullLast + 1, 0, kEncodeSize - 1);

  auto compare = [&](char* left, char* right) {
    return std::memcmp(left, right, kEncodeSize);
  };

  ascNullsFirstEncoder().encode(nullValue, encoded, kEncodeSize, true);
  ASSERT_EQ(compare(nullFirst, encoded), 0);
  ascNullsLastEncoder().encode(nullValue, encoded, kEncodeSize, true);
  ASSERT_EQ(compare(nullLast, encoded), 0);

  ascNullsFirstEncoder().encode(value, encoded, kEncodeSize, true);
  ASSERT_EQ(encoded[0], 1);
  ASSERT_EQ(std::memcmp(encoded + 1, expectedAsc, kEncodeSize - 1), 0);
  ascNullsLastEncoder().encode(value, encoded, kEncodeSize, true);
  ASSERT_EQ(encoded[0], 0);
  ASSERT_EQ(std::memcmp(encoded + 1, expectedAsc, kEncodeSize - 1), 0);
  descNullsFirstEncoder().encode(value, encoded, kEncodeSize, true);
  ASSERT_EQ(encoded[0], 1);
  ASSERT_EQ(std::memcmp(encoded + 1, expectedDesc, kEncodeSize - 1), 0);
  descNullsLastEncoder().encode(value, encoded, kEncodeSize, true);
  ASSERT_EQ(encoded[0], 0);
  ASSERT_EQ(std::memcmp(encoded + 1, expectedDesc, kEncodeSize - 1), 0);
}

TEST_F(PrefixEncoderTest, encodeWithColumnNoNulls) {
  {
    uint64_t ascExpected = 0x8877665544332211;
    uint64_t descExpected = 0x778899aabbccddee;
    testEncodeWithColumnNoNulls<uint64_t>(
        0x1122334455667788, (char*)&ascExpected, (char*)&descExpected);
  }

  {
    int64_t ascExpected = 0x8877665544332291;
    int64_t descExpected = 0x778899aabbccdd6e;
    testEncodeWithColumnNoNulls<int64_t>(
        0x1122334455667788, (char*)&ascExpected, (char*)&descExpected);
  }
  {
    uint32_t ascExpected = 0x44332211;
    uint32_t descExpected = 0xbbccddee;
    testEncodeWithColumnNoNulls<uint32_t>(
        0x11223344, (char*)&ascExpected, (char*)&descExpected);
  }
  {
    int32_t ascExpected = 0x44332291;
    int32_t descExpected = 0xbbccdd6e;
    testEncodeWithColumnNoNulls<int32_t>(
        0x11223344, (char*)&ascExpected, (char*)&descExpected);
  }

  {
    uint16_t ascExpected = 0x2211;
    uint16_t descExpected = 0xddee;
    testEncodeWithColumnNoNulls<uint16_t>(
        0x1122, (char*)&ascExpected, (char*)&descExpected);
  }
  {
    int16_t ascExpected = 0x2291;
    int16_t descExpected = 0xdd6e;
    testEncodeWithColumnNoNulls<int16_t>(
        0x1122, (char*)&ascExpected, (char*)&descExpected);
  }

  {
    uint32_t ascExpected = 0x0050c3c7;
    uint32_t descExpected = 0xffaf3c38;
    testEncodeWithColumnNoNulls<float>(
        100000.00, (char*)&ascExpected, (char*)&descExpected);
  }

  {
    uint64_t ascExpected = 0x00000000006af8c0;
    uint64_t descExpected = 0xffffffffff95073f;
    testEncodeWithColumnNoNulls<double>(
        100000.00, (char*)&ascExpected, (char*)&descExpected);
  }

  {
    char ascExpected[16] = {
        -128, 0, 0, 0, 0, 0, 0, 10, 0, 0, 0, 0, 0, 0, 0, -56};
    char descExpected[16] = {
        127, -1, -1, -1, -1, -1, -1, -11, -1, -1, -1, -1, -1, -1, -1, 55};
    int128_t value = HugeInt::build(10, 200);
    testEncodeWithColumnNoNulls<int128_t>(
        value, (char*)ascExpected, (char*)descExpected);
  }

  {
    Timestamp value = Timestamp(0x000000011223344, 0x000000011223344);
    uint64_t ascExpected[2];
    uint64_t descExpected[2];
    ascExpected[0] = 0x4433221100000080;
    ascExpected[1] = 0x4433221100000000;
    descExpected[0] = 0xbbccddeeffffff7f;
    descExpected[1] = 0xbbccddeeffffffff;
    testEncodeWithColumnNoNulls<Timestamp>(
        value, (char*)ascExpected, (char*)descExpected);
  }
}

TEST_F(PrefixEncoderTest, compare) {
  testCompare<uint64_t>();
  testCompare<uint32_t>();
  testCompare<uint16_t>();
  testCompare<int64_t>();
  testCompare<int32_t>();
  testCompare<int16_t>();
  testCompare<int128_t>();
  testCompare<float>();
  testCompare<double>();
  testCompare<Timestamp>();
}

TEST_F(PrefixEncoderTest, fuzzySmallInt) {
  testFuzz<TypeKind::SMALLINT>();
}

TEST_F(PrefixEncoderTest, fuzzyInteger) {
  testFuzz<TypeKind::INTEGER>();
}

TEST_F(PrefixEncoderTest, fuzzyBigint) {
  testFuzz<TypeKind::BIGINT>();
}

TEST_F(PrefixEncoderTest, fuzzyHugeInt) {
  testFuzz<TypeKind::HUGEINT>();
}

TEST_F(PrefixEncoderTest, fuzzyReal) {
  testFuzz<TypeKind::REAL>();
}

TEST_F(PrefixEncoderTest, fuzzyDouble) {
  testFuzz<TypeKind::DOUBLE>();
}

TEST_F(PrefixEncoderTest, fuzzyTimestamp) {
  testFuzz<TypeKind::TIMESTAMP>();
}

TEST_F(PrefixEncoderTest, fuzzyStringView) {
  testFuzz<TypeKind::VARCHAR>();
}

TEST_F(PrefixEncoderTest, fuzzyBinary) {
  testFuzz<TypeKind::VARBINARY>();
}

} // namespace facebook::velox::exec::prefixsort::test
