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

#include <gtest/gtest.h>

#include "velox/common/base/tests/GTestUtils.h"
#include "velox/type/DecimalUtil.h"

namespace facebook::velox {
namespace {

template <typename TInput>
void assertRescaleFloatingPoint(
    TInput value,
    const TypePtr& type,
    int128_t expectedValue) {
  SCOPED_TRACE(fmt::format("value: {}, type: {}", value, type->toString()));
  const auto [precision, scale] = getDecimalPrecisionScale(*type);
  int128_t actualValue;
  Status status;
  if (precision > ShortDecimalType::kMaxPrecision) {
    int128_t result;
    status = DecimalUtil::rescaleFloatingPoint<TInput, int128_t>(
        value, precision, scale, result);
    actualValue = result;
  } else {
    int64_t result;
    status = DecimalUtil::rescaleFloatingPoint<TInput, int64_t>(
        value, precision, scale, result);
    actualValue = result;
  }
  ASSERT_TRUE(status.ok());
  ASSERT_EQ(actualValue, expectedValue);
}

void assertRescaleDouble(
    double value,
    const TypePtr& type,
    int128_t expectedValue) {
  assertRescaleFloatingPoint<double>(value, type, expectedValue);
}

void assertRescaleReal(
    float value,
    const TypePtr& type,
    int128_t expectedValue) {
  assertRescaleFloatingPoint<float>(value, type, expectedValue);
}

template <typename TInput>
void assertRescaleFloatingPointFail(
    TInput value,
    const TypePtr& type,
    const std::string& expectedErrorMessage) {
  SCOPED_TRACE(fmt::format("value: {}, type: {}", value, type->toString()));
  const auto [precision, scale] = getDecimalPrecisionScale(*type);
  if (precision > ShortDecimalType::kMaxPrecision) {
    int128_t result;
    VELOX_ASSERT_ERROR_STATUS(
        (DecimalUtil::rescaleFloatingPoint<TInput, int128_t>(
            value, precision, scale, result)),
        StatusCode::kUserError,
        expectedErrorMessage);
  } else {
    int64_t result;
    VELOX_ASSERT_ERROR_STATUS(
        (DecimalUtil::rescaleFloatingPoint<TInput, int64_t>(
            value, precision, scale, result)),
        StatusCode::kUserError,
        expectedErrorMessage);
  }
}

void assertRescaleDoubleFail(
    double value,
    const TypePtr& type,
    const std::string& expectedErrorMessage) {
  assertRescaleFloatingPointFail<double>(value, type, expectedErrorMessage);
}

void assertRescaleRealFail(
    float value,
    const TypePtr& type,
    const std::string& expectedErrorMessage) {
  assertRescaleFloatingPointFail<float>(value, type, expectedErrorMessage);
}

void testToByteArray(int128_t value, int8_t* expected, int32_t size) {
  std::vector<char> out(size);
  int32_t length = DecimalUtil::toByteArray(value, out.data());
  EXPECT_EQ(length, size);
  EXPECT_EQ(DecimalUtil::getByteArrayLength(value), size);
  EXPECT_EQ(std::memcmp(expected, out.data(), length), 0);
}

template <typename T>
void testcastToString(
    T unscaleValue,
    int precision,
    int scale,
    int maxStringSize,
    const std::string& expected) {
  std::vector<char> out(maxStringSize);
  auto actualSize = DecimalUtil::castToString<T>(
      unscaleValue, scale, maxStringSize, out.data());
  EXPECT_EQ(expected.size(), actualSize);
  EXPECT_EQ(std::memcmp(expected.data(), out.data(), expected.size()), 0);
}

void testMaxStringViewSize(
    int precision,
    int scale,
    int expectedMaxStringSize) {
  EXPECT_EQ(
      DecimalUtil::maxStringViewSize(precision, scale), expectedMaxStringSize);
}

template <typename T>
void testCastFromString(
    const std::vector<std::string>& inputs,
    int toPrecision,
    int toScale,
    const std::vector<T>& expectedUnscaleValues) {
  for (int i = 0; i < inputs.size(); ++i) {
    SCOPED_TRACE(fmt::format(
        "Index: {}, input: {}, expectedUnscaleValue: {}.",
        i,
        inputs[i],
        expectedUnscaleValues[i]));
    T decimalValue;
    auto status = DecimalUtil::castFromString<T>(
        StringView(inputs[i]), toPrecision, toScale, decimalValue);
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(expectedUnscaleValues[i], decimalValue);
  }
}

template <typename T>
void testCastFromString(
    const std::string& input,
    int toPrecision,
    int toScale,
    const std::string& expectedError) {
  T decimalValue;
  auto status = DecimalUtil::castFromString<T>(
      StringView(input), toPrecision, toScale, decimalValue);
  EXPECT_EQ(status, Status::UserError(expectedError));
}

std::string zeros(uint32_t numZeros) {
  return std::string(numZeros, '0');
}

TEST(DecimalTest, toString) {
  EXPECT_EQ(std::to_string(HugeInt::build(0, 0)), "0");
  EXPECT_EQ(std::to_string(HugeInt::build(0, 1)), "1");
  EXPECT_EQ(
      std::to_string(
          HugeInt::build(0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull)),
      "-1");
  EXPECT_EQ(std::to_string(HugeInt::build(1, 0)), "18446744073709551616");
  EXPECT_EQ(
      std::to_string(HugeInt::build(0xFFFFFFFFFFFFFFFFull, 0)),
      "-18446744073709551616");
  constexpr int128_t kMax =
      HugeInt::build(0x7FFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull);
  EXPECT_EQ(std::to_string(kMax), "170141183460469231731687303715884105727");
  EXPECT_EQ(
      std::to_string(-kMax - 1), "-170141183460469231731687303715884105728");
}

TEST(DecimalTest, decimalToString) {
  ASSERT_EQ("1000", DecimalUtil::toString(1000, DECIMAL(10, 0)));
  ASSERT_EQ("1.000", DecimalUtil::toString(1000, DECIMAL(10, 3)));
  ASSERT_EQ("0.001000", DecimalUtil::toString(1000, DECIMAL(10, 6)));
  ASSERT_EQ("-0.001000", DecimalUtil::toString(-1000, DECIMAL(10, 6)));
  ASSERT_EQ("-123.451000", DecimalUtil::toString(-123451000, DECIMAL(10, 6)));

  ASSERT_EQ("1000", DecimalUtil::toString(1000, DECIMAL(20, 0)));
  ASSERT_EQ("1.000", DecimalUtil::toString(1000, DECIMAL(20, 3)));
  ASSERT_EQ("0.0000001000", DecimalUtil::toString(1000, DECIMAL(20, 10)));
  ASSERT_EQ("-0.001000", DecimalUtil::toString(-1000, DECIMAL(20, 6)));
  ASSERT_EQ("0.000000000", DecimalUtil::toString(0, DECIMAL(20, 9)));

  const auto minShortDecimal =
      DecimalUtil::toString(DecimalUtil::kShortDecimalMin, DECIMAL(18, 0));
  ASSERT_EQ("-999999999999999999", minShortDecimal);
  // Additional 1 for negative sign.
  ASSERT_EQ(minShortDecimal.length(), 19);

  const auto maxShortDecimal =
      DecimalUtil::toString(DecimalUtil::kShortDecimalMax, DECIMAL(18, 0));
  ASSERT_EQ("999999999999999999", maxShortDecimal);
  ASSERT_EQ(maxShortDecimal.length(), 18);

  const auto minLongDecimal =
      DecimalUtil::toString(DecimalUtil::kLongDecimalMin, DECIMAL(38, 0));
  ASSERT_EQ("-99999999999999999999999999999999999999", minLongDecimal);
  // Additional 1 for negative sign.
  ASSERT_EQ(minLongDecimal.length(), 39);

  const auto maxLongDecimal =
      DecimalUtil::toString(DecimalUtil::kLongDecimalMax, DECIMAL(38, 0));
  ASSERT_EQ("99999999999999999999999999999999999999", maxLongDecimal);
  ASSERT_EQ(maxLongDecimal.length(), 38);
}

TEST(DecimalTest, limits) {
  VELOX_ASSERT_THROW(
      DecimalUtil::valueInRange(DecimalUtil::kLongDecimalMax + 1),
      "Value '100000000000000000000000000000000000000' is not in the range of Decimal Type");
  VELOX_ASSERT_THROW(
      DecimalUtil::valueInRange(DecimalUtil::kLongDecimalMin - 1),
      "Value '-100000000000000000000000000000000000000' is not in the range of Decimal Type");
}

TEST(DecimalTest, addUnsignedValues) {
  int128_t a = -HugeInt::build(0x4B3B4CA85A86C47A, 0x98A223FFFFFFFFF);
  int128_t sum = a;
  int64_t overflow = 0;
  auto count = 1'000'000;
  // Test underflow
  for (int i = 1; i < count; ++i) {
    overflow += DecimalUtil::addWithOverflow(sum, a, sum);
  }
  ASSERT_EQ(-587747, overflow);
  ASSERT_EQ(HugeInt::upper(sum), 0xE98C20AD1C80DBEF);
  ASSERT_EQ(HugeInt::lower(sum), 0xFEE2F000000F4240);

  // Test overflow.
  overflow = 0;
  a = -a;
  sum = a;
  for (int i = 1; i < count; ++i) {
    overflow += DecimalUtil::addWithOverflow(sum, a, sum);
  }
  ASSERT_EQ(587747, overflow);
  ASSERT_EQ(HugeInt::upper(sum), 0x1673df52e37f2410);
  ASSERT_EQ(HugeInt::lower(sum), 0x11d0ffffff0bdc0);
}

TEST(DecimalTest, longDecimalSerDe) {
  char data[100];
  HugeInt::serialize(DecimalUtil::kLongDecimalMin, data);
  auto deserializedData = HugeInt::deserialize(data);
  ASSERT_EQ(deserializedData, DecimalUtil::kLongDecimalMin);

  HugeInt::serialize(DecimalUtil::kLongDecimalMax, data);
  deserializedData = HugeInt::deserialize(data);
  ASSERT_EQ(deserializedData, DecimalUtil::kLongDecimalMax);

  HugeInt::serialize(-1, data);
  deserializedData = HugeInt::deserialize(data);
  ASSERT_EQ(deserializedData, -1);

  HugeInt::serialize(10, data);
  deserializedData = HugeInt::deserialize(data);
  ASSERT_EQ(deserializedData, 10);
}

// The result can be obtained by
// test("biginteger") {
//   val a = new BigInteger("20")
//   val arr = a.toByteArray
//   print("length is " + arr.length + "\n")
//   arr.foreach(r => print(r + ","))
// }
TEST(DecimalTest, toByteArray) {
  int8_t expected0[1] = {0};
  testToByteArray(0, expected0, 1);

  int8_t expected1[1] = {20};
  testToByteArray(20, expected1, 1);

  int8_t expected2[1] = {-20};
  testToByteArray(-20, expected2, 1);

  int8_t expected3[2] = {0, -56};
  testToByteArray(200, expected3, 2);

  int8_t expected4[2] = {78, 32};
  testToByteArray(20000, expected4, 2);

  int8_t expected5[6] = {-2, -32, -114, 4, -5, 77};
  testToByteArray(-1234567890099, expected5, 6);

  int8_t expected6[8] = {13, -32, -74, -77, -89, 99, -1, -1};
  testToByteArray(DecimalUtil::kShortDecimalMax, expected6, 8);

  int8_t expected7[16] = {
      -76, -60, -77, 87, -91, 121, 59, -123, -10, 117, -35, -64, 0, 0, 0, 1};
  testToByteArray(DecimalUtil::kLongDecimalMin, expected7, 16);

  int8_t expected8[16] = {
      75, 59, 76, -88, 90, -122, -60, 122, 9, -118, 34, 63, -1, -1, -1, -1};
  testToByteArray(DecimalUtil::kLongDecimalMax, expected8, 16);
}

TEST(DecimalTest, valueInPrecisionRange) {
  ASSERT_TRUE(DecimalUtil::valueInPrecisionRange<int64_t>(12, 3));
  ASSERT_TRUE(DecimalUtil::valueInPrecisionRange<int64_t>(999, 3));
  ASSERT_FALSE(DecimalUtil::valueInPrecisionRange<int64_t>(1000, 3));
  ASSERT_FALSE(DecimalUtil::valueInPrecisionRange<int64_t>(1234, 3));
  ASSERT_TRUE(DecimalUtil::valueInPrecisionRange<int64_t>(
      DecimalUtil::kShortDecimalMax, ShortDecimalType::kMaxPrecision));
  ASSERT_FALSE(DecimalUtil::valueInPrecisionRange<int64_t>(
      DecimalUtil::kShortDecimalMax + 1, ShortDecimalType::kMaxPrecision));
  ASSERT_TRUE(DecimalUtil::valueInPrecisionRange<int128_t>(
      DecimalUtil::kLongDecimalMax, LongDecimalType::kMaxPrecision));
  ASSERT_FALSE(DecimalUtil::valueInPrecisionRange<int128_t>(
      DecimalUtil::kLongDecimalMax + 1, LongDecimalType::kMaxPrecision));
  ASSERT_FALSE(DecimalUtil::valueInPrecisionRange<int128_t>(
      DecimalUtil::kLongDecimalMin - 1, LongDecimalType::kMaxPrecision));
}

TEST(DecimalTest, computeAverage) {
  auto validateSameValues = [](int128_t value, int64_t maxCount) {
    SCOPED_TRACE(fmt::format("value={} maxCount={}", value, maxCount));
    int128_t sum = 0;
    int64_t overflow = 0;
    for (int64_t i = 1; i <= maxCount; ++i) {
      overflow += DecimalUtil::addWithOverflow(sum, sum, value);
      int128_t avg;
      DecimalUtil::computeAverage(avg, sum, i, overflow);
      ASSERT_EQ(avg, value);
    }
  };
  validateSameValues(DecimalUtil::kLongDecimalMin, 1'000'000);
  validateSameValues(DecimalUtil::kLongDecimalMax, 1'000'000);
}

TEST(DecimalAggregateTest, adjustSumForOverflow) {
  struct SumWithOverflow {
    int128_t sum{0};
    int64_t overflow{0};

    void add(int128_t input) {
      overflow += DecimalUtil::addWithOverflow(sum, sum, input);
    }

    std::optional<int128_t> adjustedSum() const {
      return DecimalUtil::adjustSumForOverflow(sum, overflow);
    }

    void reset() {
      sum = 0;
      overflow = 0;
    }
  };

  SumWithOverflow accumulator;
  // kLongDecimalMax + kLongDecimalMax will trigger one upward overflow, and the
  // final sum result calculated by DecimalUtil::addWithOverflow is negative.
  // DecimalUtil::adjustSumForOverflow can adjust the sum to kLongDecimalMax
  // correctly.
  accumulator.add(DecimalUtil::kLongDecimalMax);
  accumulator.add(DecimalUtil::kLongDecimalMax);
  accumulator.add(DecimalUtil::kLongDecimalMin);
  EXPECT_EQ(accumulator.adjustedSum(), DecimalUtil::kLongDecimalMax);

  accumulator.reset();
  // kLongDecimalMin + kLongDecimalMin will trigger one downward overflow, and
  // the final sum result calculated by DecimalUtil::addWithOverflow is
  // positive. DecimalUtil::adjustSumForOverflow can adjust the sum to
  // kLongDecimalMin correctly.
  accumulator.add(DecimalUtil::kLongDecimalMin);
  accumulator.add(DecimalUtil::kLongDecimalMin);
  accumulator.add(DecimalUtil::kLongDecimalMax);
  EXPECT_EQ(accumulator.adjustedSum(), DecimalUtil::kLongDecimalMin);

  accumulator.reset();
  // These inputs will eventually trigger an upward overflow, and
  // DecimalUtil::adjustSumForOverflow will return std::nullopt.
  accumulator.add(DecimalUtil::kLongDecimalMax);
  accumulator.add(DecimalUtil::kLongDecimalMax);
  EXPECT_FALSE(accumulator.adjustedSum().has_value());

  accumulator.reset();
  // These inputs will eventually trigger a downward overflow, and
  // DecimalUtil::adjustSumForOverflow will return std::nullopt.
  accumulator.add(DecimalUtil::kLongDecimalMin);
  accumulator.add(DecimalUtil::kLongDecimalMin);
  EXPECT_FALSE(accumulator.adjustedSum().has_value());
}

TEST(DecimalTest, rescaleDouble) {
  assertRescaleDouble(-3333.03, DECIMAL(10, 4), -33'330'300);
  assertRescaleDouble(-3333.03, DECIMAL(20, 1), -33'330);
  assertRescaleDouble(
      -3333.03, DECIMAL(38, 18), HugeInt::parse("-333303" + zeros(16)));

  assertRescaleDouble(-2222.02, DECIMAL(10, 4), -22'220'200);
  assertRescaleDouble(-2222.02, DECIMAL(20, 1), -22'220);
  assertRescaleDouble(
      -2222.02, DECIMAL(38, 18), HugeInt::parse("-222202" + zeros(16)));

  assertRescaleDouble(-1.0, DECIMAL(10, 4), -10'000);
  assertRescaleDouble(-1.0, DECIMAL(20, 1), -10);
  assertRescaleDouble(-1.0, DECIMAL(38, 18), -1'000'000'000'000'000'000);

  assertRescaleDouble(0.00, DECIMAL(10, 4), 0);
  assertRescaleDouble(0.00, DECIMAL(20, 1), 0);
  assertRescaleDouble(0.00, DECIMAL(38, 18), 0);

  assertRescaleDouble(100, DECIMAL(10, 4), 1'000'000);
  assertRescaleDouble(100, DECIMAL(20, 1), 1'000);
  assertRescaleDouble(100, DECIMAL(38, 18), HugeInt::parse("100" + zeros(18)));

  assertRescaleDouble(99999.99, DECIMAL(10, 4), 999'999'900);
  assertRescaleDouble(99999.99, DECIMAL(20, 1), 1'000'000);
  assertRescaleDouble(
      99999.99, DECIMAL(38, 18), HugeInt::parse("9999999" + zeros(16)));

  assertRescaleDouble(0.95, DECIMAL(3, 1), 10);
  assertRescaleDouble(
      10.03, DECIMAL(38, 18), HugeInt::parse("1003" + zeros(16)));
  assertRescaleDouble(0.034567890, DECIMAL(38, 18), 34'567'890'000'000'000);
  assertRescaleDouble(
      0.03456789, DECIMAL(38, 33), HugeInt::parse("3456789" + zeros(25)));
  assertRescaleDouble(
      0.999999999999999, DECIMAL(38, 18), 999'999'999'999'999'000);
  assertRescaleDouble(
      0.123456789123123, DECIMAL(38, 18), 123'456'789'123'123'000);
  assertRescaleDouble(21.54551, DECIMAL(12, 3), 21546);

  assertRescaleDouble(std::numeric_limits<double>::min(), DECIMAL(38, 2), 0);

  assertRescaleDouble(0.9999999999999999, DECIMAL(17, 2), 100);

  assertRescaleDouble(-0.9999999999999999, DECIMAL(17, 2), -100);

  assertRescaleDouble(
      kMaxDoubleBelowInt64Max, DECIMAL(19, 0), 9'223'372'036'854'774'784);

  // Test for overflows.
  std::vector<double> invalidInputs = {
      9999999999999999999999.99,
      static_cast<double>(
          static_cast<int128_t>(std::numeric_limits<int64_t>::max()) + 1),
      static_cast<double>(
          static_cast<int128_t>(std::numeric_limits<int64_t>::min()) - 1),
      static_cast<double>(DecimalUtil::kShortDecimalMax),
      static_cast<double>(DecimalUtil::kShortDecimalMin),
      static_cast<double>(DecimalUtil::kLongDecimalMax),
      static_cast<double>(DecimalUtil::kLongDecimalMin),
      std::numeric_limits<double>::max(),
      std::numeric_limits<double>::lowest()};
  std::vector<TypePtr> toTypes = {
      DECIMAL(10, 2),
      DECIMAL(18, 0),
      DECIMAL(18, 0),
      DECIMAL(10, 2),
      DECIMAL(10, 2),
      DECIMAL(20, 2),
      DECIMAL(20, 2),
      DECIMAL(38, 0),
      DECIMAL(38, 0)};
  for (int32_t i = 0; i < invalidInputs.size(); i++) {
    assertRescaleDoubleFail(invalidInputs[i], toTypes[i], "Result overflows.");
  }

  assertRescaleDoubleFail(
      NAN, DECIMAL(10, 2), "The input value should be finite.");
  assertRescaleDoubleFail(
      INFINITY, DECIMAL(10, 2), "The input value should be finite.");
  assertRescaleDoubleFail(
      99999.99, DECIMAL(6, 4), "Result cannot fit in the given precision 6.");
}

TEST(DecimalTest, rescaleReal) {
  assertRescaleReal(-3333.03, DECIMAL(10, 4), -33'330'300);
  assertRescaleReal(-3333.03, DECIMAL(20, 1), -33'330);
  assertRescaleReal(
      -3333.03, DECIMAL(38, 18), HugeInt::parse("-333303" + zeros(16)));

  assertRescaleReal(-2222.02, DECIMAL(10, 4), -22'220'200);
  assertRescaleReal(-2222.02, DECIMAL(20, 1), -22'220);
  assertRescaleReal(
      -2222.02, DECIMAL(38, 18), HugeInt::parse("-222202" + zeros(16)));

  assertRescaleReal(-1.0, DECIMAL(10, 4), -10'000);
  assertRescaleReal(-1.0, DECIMAL(20, 1), -10);
  assertRescaleReal(-1.0, DECIMAL(38, 18), -1'000'000'000'000'000'000);

  assertRescaleReal(0.00, DECIMAL(10, 4), 0);
  assertRescaleReal(0.00, DECIMAL(20, 1), 0);
  assertRescaleReal(0.00, DECIMAL(38, 18), 0);

  assertRescaleReal(100, DECIMAL(10, 4), 1'000'000);
  assertRescaleReal(100, DECIMAL(20, 1), 1'000);
  assertRescaleReal(100, DECIMAL(38, 18), HugeInt::parse("100" + zeros(18)));

  assertRescaleReal(9999.99, DECIMAL(10, 4), 99'999'900);
  assertRescaleReal(9999.99, DECIMAL(20, 1), 100'000);
  assertRescaleReal(
      9999.99, DECIMAL(38, 18), HugeInt::parse("999999" + zeros(16)));

  assertRescaleReal(0.95, DECIMAL(3, 1), 10);
  assertRescaleReal(10.03, DECIMAL(38, 18), HugeInt::parse("1003" + zeros(16)));
  assertRescaleReal(0.034567, DECIMAL(38, 18), 34'567'000'000'000'000);
  assertRescaleReal(
      0.999999999999999, DECIMAL(38, 18), 1'000'000'000'000'000'000);
  assertRescaleReal(0.123456, DECIMAL(38, 18), 123'456'000'000'000'000);
  assertRescaleReal(21.5455, DECIMAL(12, 3), 21546);

  assertRescaleReal(std::numeric_limits<float>::min(), DECIMAL(38, 2), 0);

  assertRescaleReal(27867.64, DECIMAL(18, 2), 2786764);
  assertRescaleReal(27867.644, DECIMAL(18, 2), 2786764);
  assertRescaleReal(27867.645, DECIMAL(18, 2), 2786764);

  // Test for overflows.
  std::vector<float> invalidInputs = {
      std::numeric_limits<float>::max(),
      std::numeric_limits<float>::lowest(),
      9999999999999999999999.99,
      static_cast<float>(
          static_cast<int128_t>(std::numeric_limits<int64_t>::max()) + 1),
      static_cast<float>(
          static_cast<int128_t>(std::numeric_limits<int64_t>::min()) - 1),
      static_cast<float>(DecimalUtil::kShortDecimalMax),
      static_cast<float>(DecimalUtil::kShortDecimalMin),
      static_cast<float>(DecimalUtil::kLongDecimalMax),
      static_cast<float>(DecimalUtil::kLongDecimalMin),
  };
  std::vector<TypePtr> toTypes = {
      DECIMAL(38, 0),
      DECIMAL(38, 0),
      DECIMAL(10, 2),
      DECIMAL(18, 0),
      DECIMAL(18, 0),
      DECIMAL(10, 2),
      DECIMAL(10, 2),
      DECIMAL(20, 2),
      DECIMAL(20, 2)};
  for (int32_t i = 0; i < invalidInputs.size(); i++) {
    assertRescaleRealFail(invalidInputs[i], toTypes[i], "Result overflows.");
  }

  assertRescaleRealFail(
      99999.99, DECIMAL(6, 4), "Result cannot fit in the given precision 6.");
  assertRescaleRealFail(
      NAN, DECIMAL(10, 2), "The input value should be finite.");
  assertRescaleRealFail(
      INFINITY, DECIMAL(10, 2), "The input value should be finite.");
}

TEST(DecimalTest, maxStringViewSize) {
  testMaxStringViewSize(10, 0, 11);
  testMaxStringViewSize(10, 1, 12);
  testMaxStringViewSize(10, 10, 13);
}

TEST(DecimalTest, castToString) {
  testcastToString<int64_t>(12, 10, 0, 11, "12");
  testcastToString<int64_t>(12, 10, 1, 12, "1.2");
  testcastToString<int64_t>(12, 10, 3, 12, "0.012");
  testcastToString<int64_t>(-12, 10, 3, 12, "-0.012");
  testcastToString<int64_t>(12, 5, 5, 8, "0.00012");
  testcastToString<int64_t>(-12, 5, 5, 8, "-0.00012");
  testcastToString<int64_t>(-12, 5, 5, 8, "-0.00012");
  testcastToString<int64_t>(
      DecimalUtil::kShortDecimalMax, 18, 0, 19, std::string(18, '9'));
  testcastToString<int64_t>(
      DecimalUtil::kShortDecimalMin, 18, 0, 19, "-" + std::string(18, '9'));

  testcastToString<int128_t>(
      HugeInt::parse("-18446744073709551616"),
      20,
      0,
      21,
      "-18446744073709551616");

  testcastToString<int128_t>(
      HugeInt::parse("-18446744073709551616"),
      20,
      3,
      22,
      "-18446744073709551.616");

  testcastToString<int128_t>(
      HugeInt::parse("-12345678901234567890"),
      20,
      20,
      23,
      "-0.12345678901234567890");
  testcastToString<int128_t>(
      DecimalUtil::kLongDecimalMax, 38, 0, 39, std::string(38, '9'));
  testcastToString<int128_t>(
      DecimalUtil::kLongDecimalMin, 38, 0, 39, "-" + std::string(38, '9'));
}

TEST(DecimalTest, castFromString) {
  testCastFromString<int64_t>(
      {"9999999999.99",
       "15",
       "1.5",
       "-1.5",
       "1.556",
       "1.554",
       ("1.556" + std::string(32, '1')).data(),
       ("1.556" + std::string(32, '9')).data(),
       "0000.123",
       ".12300000000",
       "+09",
       "9.",
       ".9",
       "3E2",
       "-3E+2",
       "3E+2",
       "3E+00002",
       "3E-2",
       "3e+2",
       "3e-2",
       "3.5E-2",
       "3.4E-2",
       "3.5E+2",
       "3.4E+2",
       "31.423e+2",
       "31.423e-2",
       "31.523e-2",
       "-3E-00000"},
      12,
      2,
      {999'999'999'999,
       1500,
       150,
       -150,
       156,
       155,
       156,
       156,
       12,
       12,
       900,
       900,
       90,
       30000,
       -30000,
       30000,
       30000,
       3,
       30000,
       3,
       4,
       3,
       35000,
       34000,
       314230,
       31,
       32,
       -300});

  // Truncates the fractional digits with exponent.
  testCastFromString<int128_t>(
      {"112345612.23e-6",
       "112345662.23e-6",
       "1.23e-6",
       "1.23e-3",
       "1.26e-3",
       "1.23456781e3",
       "1.23456789e3",
       "1.23456789123451789123456789e9",
       "1.23456789123456789123456789e9"},
      20,
      4,
      {1123456,
       1123457,
       0,
       12,
       13,
       12345678,
       12345679,
       12345678912345,
       12345678912346});

  const auto minDecimalStr = '-' + std::string(36, '9') + '.' + "99";
  const auto maxDecimalStr = std::string(36, '9') + '.' + "99";
  testCastFromString<int128_t>(
      {minDecimalStr, maxDecimalStr, "123456789012345678901234.567"},
      38,
      2,
      {
          DecimalUtil::kLongDecimalMin,
          DecimalUtil::kLongDecimalMax,
          HugeInt::build(
              669260, 10962463713375599297U), // 12345678901234567890123457
      });

  const std::string fractionLarge = "1.9" + std::string(67, '9');
  const std::string fractionLargeExp = "1.9" + std::string(67, '9') + "e2";
  const std::string fractionLargeNegExp =
      "1000.9" + std::string(67, '9') + "e-2";
  testCastFromString<int128_t>(
      {('-' + std::string(38, '9')),
       std::string(38, '9'),
       fractionLarge,
       fractionLargeExp,
       fractionLargeNegExp},
      38,
      0,
      {DecimalUtil::kLongDecimalMin, DecimalUtil::kLongDecimalMax, 2, 200, 10});
}

TEST(DecimalTest, castFromStringError) {
  testCastFromString<int128_t>(
      std::string(280, '9'), 38, 0, "Value too large.");

  // Overflows when parsing fractional digits.
  const std::string fractionOverflow = std::string(36, '9') + '.' + "23456";
  testCastFromString<int128_t>(fractionOverflow, 38, 10, "Value too large.");

  const std::string fractionRoundUp = "0." + std::string(38, '9') + "6";
  testCastFromString<int128_t>(fractionRoundUp, 38, 38, "Value too large.");

  testCastFromString<int128_t>(
      "0.0444a", 38, 0, "Value is not a number. Chars are invalid.");

  testCastFromString<int128_t>(
      "", 38, 0, "Value is not a number. Input is empty.");

  // Exponent > LongDecimalType::kMaxPrecision.
  testCastFromString<int128_t>("1.23e67", 38, 0, "Value too large.");

  // Forcing the scale to be zero overflows.
  testCastFromString<int128_t>("20908.23e35", 38, 0, "Value too large.");

  // Rescale overflows.
  testCastFromString<int128_t>(
      "111111111111111111.23", 38, 38, "Value too large.");

  testCastFromString<int128_t>(
      "23e-5d",
      38,
      0,
      "Value is not a number. Non-digit character is not allowed in the exponent part.");

  // Whitespaces.
  testCastFromString<int128_t>(
      "1. 23", 38, 0, "Value is not a number. Chars are invalid.");
  testCastFromString<int128_t>(
      "-3E+ 2",
      12,
      2,
      "Value is not a number. Non-digit character is not allowed in the exponent part.");
  testCastFromString<int128_t>(
      "1.23 ", 38, 0, "Value is not a number. Chars are invalid.");
  testCastFromString<int64_t>(
      "-3E+2 ",
      12,
      2,
      "Value is not a number. Non-digit character is not allowed in the exponent part.");
  testCastFromString<int128_t>(
      " 1.23", 38, 0, "Value is not a number. Extracted digits are empty.");
  testCastFromString<int64_t>(
      " -3E+2", 12, 2, "Value is not a number. Extracted digits are empty.");

  testCastFromString<int64_t>(
      "-3E+2.1",
      12,
      2,
      "Value is not a number. Non-digit character is not allowed in the exponent part.");

  testCastFromString<int64_t>(
      "-3E+",
      12,
      2,
      "Value is not a number. The exponent part only contains sign.");

  testCastFromString<int64_t>(
      "-3E-",
      12,
      2,
      "Value is not a number. The exponent part only contains sign.");
  testCastFromString<int64_t>(
      "9e", 12, 2, "Value is not a number. The exponent part is empty.");
  testCastFromString<int64_t>(
      "09{xi+yD", 12, 2, "Value is not a number. Chars are invalid.");
}
} // namespace
} // namespace facebook::velox
