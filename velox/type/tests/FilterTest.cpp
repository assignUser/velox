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

#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>

#include <velox/type/DecimalUtil.h>
#include "velox/expression/ExprToSubfieldFilter.h"
#include "velox/type/Filter.h"

#include <gtest/gtest.h>

using namespace facebook::velox;
using namespace facebook::velox::common;
using namespace facebook::velox::exec;

TEST(FilterTest, alwaysFalse) {
  AlwaysFalse alwaysFalse;
  EXPECT_FALSE(alwaysFalse.testInt64(1));
  EXPECT_FALSE(alwaysFalse.testInt128Range(0, 0, false));
  EXPECT_FALSE(alwaysFalse.testInt128Range(0, 0, true));
  EXPECT_FALSE(alwaysFalse.testNonNull());
  EXPECT_FALSE(alwaysFalse.testNull());

  EXPECT_EQ(
      "Filter(AlwaysFalse, deterministic, null not allowed)",
      alwaysFalse.toString());
}

TEST(FilterTest, alwaysTrue) {
  AlwaysTrue alwaysTrue;
  EXPECT_TRUE(alwaysTrue.testInt64(1));
  EXPECT_TRUE(alwaysTrue.testInt128Range(0, 0, false));
  EXPECT_TRUE(alwaysTrue.testInt128Range(0, 0, true));
  EXPECT_TRUE(alwaysTrue.testNonNull());
  EXPECT_TRUE(alwaysTrue.testNull());
  xsimd::batch<int64_t> int64s(1);
  EXPECT_EQ(
      simd::allSetBitMask<int64_t>(),
      simd::toBitMask(alwaysTrue.testValues(int64s)));
  xsimd::batch<int32_t> int32s(2);
  EXPECT_EQ(
      simd::allSetBitMask<int32_t>(),
      simd::toBitMask(alwaysTrue.testValues(int32s)));
  xsimd::batch<int16_t> int16s(3);
  EXPECT_EQ(
      simd::allSetBitMask<int16_t>(),
      simd::toBitMask(alwaysTrue.testValues(int16s)));
  EXPECT_EQ(
      "Filter(AlwaysTrue, deterministic, null allowed)", alwaysTrue.toString());
}

TEST(FilterTest, isNotNull) {
  IsNotNull notNull;
  EXPECT_TRUE(notNull.testNonNull());
  EXPECT_TRUE(notNull.testInt64(10));
  EXPECT_TRUE(notNull.testInt128Range(0, 0, false));
  EXPECT_TRUE(notNull.testInt128Range(0, 0, true));

  EXPECT_FALSE(notNull.testNull());
}

TEST(FilterTest, isNull) {
  IsNull isNull;
  EXPECT_TRUE(isNull.testNull());

  EXPECT_FALSE(isNull.testNonNull());
  EXPECT_FALSE(isNull.testInt64(10));
  EXPECT_FALSE(isNull.testInt128(10));
  EXPECT_FALSE(isNull.testInt128Range(0, 0, false));
  EXPECT_TRUE(isNull.testInt128Range(0, 0, true));

  EXPECT_EQ("Filter(IsNull, deterministic, null allowed)", isNull.toString());
}

// Applies 'filter' to all T type lanes of 'values' and compares the result to
// the ScalarTest applied to the same.
template <typename T, typename ScalarTest>
void checkSimd(const Filter* filter, const T* values, ScalarTest scalarTest) {
  auto v = xsimd::load_unaligned(values);
  auto bits = simd::toBitMask(filter->testValues(v));
  for (auto i = 0; i < decltype(v)::size; ++i) {
    auto expected = scalarTest(v.get(i));
    EXPECT_EQ(bits::isBitSet(&bits, i), expected) << "Lane " << i;
  }
}

TEST(FilterTest, bigIntRange) {
  // x = 1
  auto filter = equal(1, false);
  auto testInt64 = [&](int64_t x) { return filter->testInt64(x); };
  EXPECT_TRUE(filter->testInt64(1));

  EXPECT_FALSE(filter->testNull());
  EXPECT_FALSE(filter->testInt64(0));
  EXPECT_FALSE(filter->testInt64(11));

  EXPECT_FALSE(filter->testInt64Range(100, 150, false));
  EXPECT_FALSE(filter->testInt64Range(100, 150, true));
  EXPECT_TRUE(filter->testInt64Range(1, 10, false));
  EXPECT_TRUE(filter->testInt64Range(-10, 10, false));

  {
    int64_t n4[] = {2, 1, 1000, -1000};
    checkSimd(filter.get(), n4, testInt64);
    int32_t n8[] = {2, 1, 1000, -1000, 1, 1, 0, 1111};
    checkSimd(filter.get(), n8, testInt64);
    int16_t n16[] = {
        2, 1, 1000, -1000, 1, 1, 0, 1111, 2, 1, 1000, -1000, 1, 1, 0, 1111};
    checkSimd(filter.get(), n16, testInt64);
  }

  // x between 1 and 10
  filter = between(1, 10, false);
  EXPECT_TRUE(filter->testInt64(1));
  EXPECT_TRUE(filter->testInt64(10));

  EXPECT_FALSE(filter->testNull());
  EXPECT_FALSE(filter->testInt64(0));
  EXPECT_FALSE(filter->testInt64(11));

  {
    int64_t n4[] = {2, 1, 1000, -1000};
    checkSimd(filter.get(), n4, testInt64);
    int32_t n8[] = {2, 1, 1000, -1000, 1, 1, 0, 1111};
    checkSimd(filter.get(), n8, testInt64);
    int16_t n16[] = {
        2, 1, 1000, -1000, 1, 1, 0, 1111, 2, 1, 1000, -1000, 1, 1, 0, 1111};
    checkSimd(filter.get(), n16, testInt64);
  }

  EXPECT_FALSE(filter->testInt64Range(-150, -10, false));
  EXPECT_FALSE(filter->testInt64Range(-150, -10, true));
  EXPECT_TRUE(filter->testInt64Range(5, 20, false));
  EXPECT_TRUE(filter->testInt64Range(-10, 10, false));

  // x > 0 or null
  filter = greaterThan(0, true);
  EXPECT_TRUE(filter->testNull());
  EXPECT_FALSE(filter->testInt64(0));
  EXPECT_TRUE(filter->testInt64(10));
  {
    int64_t n4[] = {2, 10000000000, 1000, -1000};
    checkSimd(filter.get(), n4, testInt64);
    int32_t n8[] = {2, 1, 1000, -1000, 1, 1000000000, 0, -2000000000};
    checkSimd(filter.get(), n8, testInt64);
    int16_t n16[] = {
        2,
        1,
        32000,
        -1000,
        -32000,
        1,
        0,
        1111,
        2,
        1,
        1000,
        -1000,
        1,
        1,
        0,
        1111};
    checkSimd(filter.get(), n16, testInt64);
  }
  EXPECT_FALSE(filter->testInt64Range(-100, 0, false));
  EXPECT_TRUE(filter->testInt64Range(-100, -10, true));
  EXPECT_TRUE(filter->testInt64Range(-100, 10, false));
  EXPECT_TRUE(filter->testInt64Range(-100, 10, true));

  {
    SCOPED_TRACE("Filter range outside of smaller type");
    filter = between(2147483747ll, 2147483847ll);
    int32_t n8[] = {
        1, 100, 200, 201, 2147483647, -1000, 1000000000, -2000000000};
    checkSimd(filter.get(), n8, testInt64);
    int16_t n16[] = {
        1,
        100,
        200,
        201,
        32767,
        2,
        32000,
        -1000,
        -32000,
        1111,
        1000,
        -1000,
        1,
        1,
        0,
        1111};
    checkSimd(filter.get(), n16, testInt64);
  }
}

TEST(FilterTest, negatedBigintRange) {
  auto filter = notEqual(1, false);
  EXPECT_FALSE(filter->testNull());
  EXPECT_FALSE(filter->testInt64(1));

  EXPECT_TRUE(filter->testInt64(-1));
  EXPECT_TRUE(filter->testInt64(0));
  EXPECT_TRUE(filter->testInt64(11));

  EXPECT_FALSE(filter->testInt64Range(1, 1, false));
  EXPECT_FALSE(filter->testInt64Range(1, 1, true));
  EXPECT_TRUE(filter->testInt64Range(1, 2, false));
  EXPECT_TRUE(filter->testInt64Range(8, 9, false));

  EXPECT_EQ(filter->lower(), 1);
  EXPECT_EQ(filter->upper(), 1);

  auto testInt64 = [&](int64_t x) { return filter->testInt64(x); };

  int64_t n4[] = {0, 1, 26, std::numeric_limits<int64_t>::max()};
  checkSimd(filter.get(), n4, testInt64);
  int32_t n8[] = {2, 1, 1000, -1000, 1, 15, 0, 1111};
  checkSimd(filter.get(), n8, testInt64);
  int16_t n16[] = {
      2, 1, 1000, -1000, 1, -5, 0, 1111, 2, 1, 1000, -1000, 1, 1, 0, 1111};
  checkSimd(filter.get(), n16, testInt64);

  filter = notBetween(-5, 15, false);
  EXPECT_FALSE(filter->testNull());
  EXPECT_FALSE(filter->testInt64(-5));
  EXPECT_FALSE(filter->testInt64(-1));
  EXPECT_FALSE(filter->testInt64(0));
  EXPECT_FALSE(filter->testInt64(1));
  EXPECT_FALSE(filter->testInt64(15));

  EXPECT_TRUE(filter->testInt64(-6));
  EXPECT_TRUE(filter->testInt64(16));
  EXPECT_TRUE(filter->testInt64(99));

  EXPECT_FALSE(filter->testInt64Range(-5, 15, false));
  EXPECT_FALSE(filter->testInt64Range(-5, 15, true));
  EXPECT_FALSE(filter->testInt64Range(-3, -1, false));
  EXPECT_FALSE(filter->testInt64Range(-4, 0, false));
  EXPECT_FALSE(filter->testInt64Range(0, 8, false));

  EXPECT_TRUE(filter->testInt64Range(16, 16, false));
  EXPECT_TRUE(filter->testInt64Range(15, 16, false));
  EXPECT_TRUE(filter->testInt64Range(-10, 20, false));
  EXPECT_TRUE(filter->testInt64Range(-6, 15, false));
  checkSimd(filter.get(), n4, testInt64);
  checkSimd(filter.get(), n8, testInt64);
  checkSimd(filter.get(), n16, testInt64);

  EXPECT_EQ(filter->lower(), -5);
  EXPECT_EQ(filter->upper(), 15);

  auto filter_with_null = filter->clone(true);
  EXPECT_TRUE(filter_with_null->testNull());
  EXPECT_TRUE(filter_with_null->testInt64Range(5, 15, true));
}

TEST(FilterTest, bigintValuesUsingHashTable) {
  auto filter = createBigintValues({1, 10, 100, 10'000}, false);
  ASSERT_TRUE(dynamic_cast<BigintValuesUsingHashTable*>(filter.get()));

  EXPECT_TRUE(filter->testInt64(1));
  EXPECT_TRUE(filter->testInt64(10));
  EXPECT_TRUE(filter->testInt64(100));
  EXPECT_TRUE(filter->testInt64(10'000));

  EXPECT_FALSE(filter->testNull());
  EXPECT_FALSE(filter->testInt64(-1));
  EXPECT_FALSE(filter->testInt64(2));
  EXPECT_FALSE(filter->testInt64(102));
  EXPECT_FALSE(filter->testInt64(INT64_MAX));

  EXPECT_TRUE(filter->testInt64Range(5, 50, false));
  EXPECT_FALSE(filter->testInt64Range(11, 11, false));
  EXPECT_FALSE(filter->testInt64Range(-10, -5, false));
  EXPECT_FALSE(filter->testInt64Range(10'234, 20'000, false));
  EXPECT_FALSE(filter->testInt64(102));
  EXPECT_FALSE(filter->testInt64(INT64_MAX));

  EXPECT_TRUE(filter->testInt64Range(5, 50, false));
  EXPECT_FALSE(filter->testInt64Range(11, 11, false));
  EXPECT_FALSE(filter->testInt64Range(15, 17, false)); // 15, 16, 17 all false
  EXPECT_FALSE(filter->testInt64Range(-10, -5, false));
  EXPECT_TRUE(filter->testInt64Range(10'000, 20'000, false));
  EXPECT_FALSE(filter->testInt64Range(9'000, 9'999, false));
  EXPECT_TRUE(filter->testInt64Range(9'000, 10'000, false));
  EXPECT_TRUE(filter->testInt64Range(0, 1, false));
}

TEST(FilterTest, negatedBigintValuesUsingHashTableOverflow) {
  auto filter = createNegatedBigintValues(
      {-9074444101981834051, 7013258837215469735}, false);
  EXPECT_TRUE(
      filter->testInt64Range(-9074444101981834051, 7013258837215469735, false));
}

TEST(FilterTest, negatedBigintValuesUsingHashTable) {
  auto filter = createNegatedBigintValues({1, 6, 10'000, 8, 9, 100, 10}, false);
  auto castedFilter =
      dynamic_cast<NegatedBigintValuesUsingHashTable*>(filter.get());
  ASSERT_TRUE(castedFilter);
  std::vector<int64_t> filterVals = {1, 6, 8, 9, 10, 100, 10'000};
  ASSERT_EQ(castedFilter->values(), filterVals);
  ASSERT_EQ(castedFilter->min(), 1);
  ASSERT_EQ(castedFilter->max(), 10'000);

  EXPECT_FALSE(filter->testInt64(1));
  EXPECT_FALSE(filter->testInt64(10));
  EXPECT_FALSE(filter->testInt64(100));
  EXPECT_FALSE(filter->testInt64(10'000));
  EXPECT_FALSE(filter->testNull());

  EXPECT_TRUE(filter->testInt64(-1));
  EXPECT_TRUE(filter->testInt64(2));
  EXPECT_TRUE(filter->testInt64(102));
  EXPECT_TRUE(filter->testInt64(0xdeadbeefbadefeed));
  EXPECT_TRUE(filter->testInt64(INT64_MAX));

  EXPECT_TRUE(filter->testInt64Range(5, 50, false));
  EXPECT_TRUE(filter->testInt64Range(11, 11, false));
  EXPECT_TRUE(filter->testInt64Range(-10, -5, false));
  EXPECT_TRUE(filter->testInt64Range(10'234, 20'000, false));
  EXPECT_TRUE(filter->testInt64Range(0, 1, false));
  EXPECT_TRUE(filter->testInt64Range(6, 10, false));
  EXPECT_TRUE(filter->testInt64Range(20, 99, false));
  EXPECT_TRUE(filter->testInt64Range(0, 1, false));
  EXPECT_FALSE(filter->testInt64Range(10, 10, false));
  EXPECT_FALSE(filter->testInt64Range(100, 100, false));
  EXPECT_FALSE(filter->testInt64Range(8, 10, false));
  EXPECT_FALSE(filter->testInt64Range(8, 9, false));
  EXPECT_FALSE(filter->testInt64Range(8, 10, true));

  auto filter_copy = filter->clone();
  EXPECT_FALSE(filter_copy->testInt64(10));
  EXPECT_FALSE(filter_copy->testInt64(100));
  EXPECT_FALSE(filter_copy->testNull());
  EXPECT_TRUE(filter_copy->testInt64(-1));
  EXPECT_TRUE(filter_copy->testInt64(2));
  EXPECT_TRUE(filter_copy->testInt64(102));
  EXPECT_TRUE(filter_copy->testInt64(0xdeadbeefbadefeed));

  EXPECT_TRUE(filter_copy->testInt64Range(5, 50, false));
  EXPECT_TRUE(filter_copy->testInt64Range(11, 11, false));
  EXPECT_FALSE(filter_copy->testInt64Range(100, 100, false));
  EXPECT_FALSE(filter_copy->testInt64Range(8, 10, false));

  auto filter_with_null = filter->clone(true);
  EXPECT_TRUE(filter_with_null->testNull());
  EXPECT_TRUE(filter_with_null->testInt64Range(8, 10, true));

  auto filter_with_null_copy = filter_with_null->clone();
  EXPECT_TRUE(filter_with_null_copy->testNull());

  auto filter_no_more_null = filter_with_null_copy->clone(false);
  EXPECT_FALSE(filter_no_more_null->testNull());
}

constexpr unsigned bitsNeeded(unsigned n) {
  return n <= 1 ? 0 : 1 + bitsNeeded((n + 1) / 2);
}

template <typename T, typename Verify>
void applySimdTestToVector(
    std::vector<T> numbers,
    const Filter& filter,
    Verify verify) {
  constexpr int kNumLanes = xsimd::batch<T>::size;
  constexpr int kBits = bitsNeeded(kNumLanes);
  for (auto i = 0; i + kNumLanes < numbers.size(); i += kNumLanes) {
    // Get keys to look up from the numbers in the filter. Make some
    // of these miss by incrementing different lanes depending on the
    // loop counter.
    auto lanes = &numbers[i];
    for (auto lane = 0; lane < kNumLanes; ++lane) {
      if (i & (1 << (lane + kBits))) {
        ++lanes[lane];
      }
    }
    checkSimd<T>(&filter, lanes, verify);
  }
}

TEST(FilterTest, bigintValuesUsingHashTableSimd) {
  std::vector<int64_t> numbers;
  // make a worst case filter where every item falls on the same slot.
  for (auto i = 0; i < 1000; ++i) {
    numbers.push_back(i * 0x10000);
  }
  auto filter = createBigintValues(numbers, false);
  ASSERT_TRUE(dynamic_cast<BigintValuesUsingHashTable*>(filter.get()));
  int64_t outOfRange[] = {-100, -20000, 0x10000000, 0x20000000};
  auto verify = [&](int64_t x) { return filter->testInt64(x); };
  checkSimd(filter.get(), outOfRange, verify);
  applySimdTestToVector(numbers, *filter, verify);
  // Make a filter with reasonably distributed entries and retry.
  numbers.clear();
  for (auto i = 0; i < 1000; ++i) {
    numbers.push_back(i * 1209);
  }
  filter = createBigintValues(numbers, false);
  ASSERT_TRUE(dynamic_cast<BigintValuesUsingHashTable*>(filter.get()));
  applySimdTestToVector(numbers, *filter, verify);

  std::vector<int32_t> numbers32(numbers.size());
  for (auto n : numbers) {
    numbers32.push_back(n);
  }

  applySimdTestToVector(numbers32, *filter, verify);

  // Make a filter with sizeMask_'s slot filled and the kPaddingElements'
  // slots tested
  // numbers has 4 elements, so the sizeMask_ is 15 (1 << log2(4*5) - 1 = 15)
  // The 15'th slot is filled by 19 (19 * 0xc6a4a7935bd1e995L & 15L = 15L)
  // The test value 3 will also be hashed to 15'th slot
  // (3 * 0xc6a4a7935bd1e995L & 15L = 15L), and 3 is within [min, max]
  numbers = {0, 1, 19, 10'000};
  filter = createBigintValues(numbers, false);
  ASSERT_TRUE(dynamic_cast<BigintValuesUsingHashTable*>(filter.get()));
  int64_t values[] = {3, 3, 3, 3};
  checkSimd(filter.get(), values, verify);
}

TEST(FilterTest, negatedBigintValuesUsingHashTableSimd) {
  std::vector<int64_t> numbers;
  // make a worst case filter where every item falls on the same slot.
  numbers.reserve(1000);
  for (auto i = 0; i < 1000; ++i) {
    numbers.push_back(i * 0x10000);
  }
  auto filter = createNegatedBigintValues(numbers, false);
  ASSERT_TRUE(dynamic_cast<NegatedBigintValuesUsingHashTable*>(filter.get()));
  int64_t outOfRange[] = {-100, -20000, 0x10000000, 0x20000000};
  auto verify = [&](int64_t x) { return filter->testInt64(x); };
  checkSimd(filter.get(), outOfRange, verify);
  applySimdTestToVector(numbers, *filter, verify);
  // Make a filter with reasonably distributed entries and retry.
  numbers.clear();
  for (auto i = 0; i < 1000; ++i) {
    numbers.push_back(i * 1209);
  }
  filter = createNegatedBigintValues(numbers, false);
  ASSERT_TRUE(dynamic_cast<NegatedBigintValuesUsingHashTable*>(filter.get()));
  applySimdTestToVector(numbers, *filter, verify);

  std::vector<int32_t> numbers32(numbers.size());
  for (auto n : numbers) {
    numbers32.push_back(n);
  }

  applySimdTestToVector(numbers32, *filter, verify);

  std::vector<int16_t> numbers16(numbers.size());
  for (auto n : numbers) {
    numbers16.push_back(n);
  }

  applySimdTestToVector(numbers16, *filter, verify);
}

TEST(FilterTest, bigintValuesUsingBitmask) {
  auto filter = createBigintValues({1, 10, 100, 1000}, false);
  auto castedFilter = dynamic_cast<BigintValuesUsingBitmask*>(filter.get());
  ASSERT_TRUE(castedFilter);
  ASSERT_EQ(castedFilter->min(), 1);
  ASSERT_EQ(castedFilter->max(), 1000);

  EXPECT_TRUE(filter->testInt64(1));
  EXPECT_TRUE(filter->testInt64(10));
  EXPECT_TRUE(filter->testInt64(100));
  EXPECT_TRUE(filter->testInt64(1000));

  EXPECT_FALSE(filter->testNull());
  EXPECT_FALSE(filter->testInt64(-1));
  EXPECT_FALSE(filter->testInt64(2));
  EXPECT_FALSE(filter->testInt64(102));
  EXPECT_FALSE(filter->testInt64(INT64_MAX));

  EXPECT_TRUE(filter->testInt64Range(5, 50, false));
  EXPECT_FALSE(filter->testInt64Range(11, 11, false));
  EXPECT_FALSE(filter->testInt64Range(-10, -5, false));
  EXPECT_FALSE(filter->testInt64Range(1234, 2000, false));
}

TEST(FilterTest, negatedBigintValuesUsingBitmask) {
  auto filter = createNegatedBigintValues({1, 6, 1000, 8, 9, 100, 10}, false);
  auto castedFilter =
      dynamic_cast<NegatedBigintValuesUsingBitmask*>(filter.get());
  ASSERT_TRUE(castedFilter);
  std::vector<int64_t> filterVals = {1, 6, 8, 9, 10, 100, 1000};
  ASSERT_EQ(castedFilter->values(), filterVals);
  ASSERT_EQ(castedFilter->min(), 1);
  ASSERT_EQ(castedFilter->max(), 1000);

  EXPECT_FALSE(filter->testInt64(1));
  EXPECT_FALSE(filter->testInt64(10));
  EXPECT_FALSE(filter->testInt64(100));
  EXPECT_FALSE(filter->testInt64(1000));
  EXPECT_FALSE(filter->testNull());

  EXPECT_TRUE(filter->testInt64(-1));
  EXPECT_TRUE(filter->testInt64(0));
  EXPECT_TRUE(filter->testInt64(2));
  EXPECT_TRUE(filter->testInt64(102));
  EXPECT_TRUE(filter->testInt64(INT64_MAX));

  EXPECT_TRUE(filter->testInt64Range(5, 50, false));
  EXPECT_TRUE(filter->testInt64Range(11, 11, false));
  EXPECT_TRUE(filter->testInt64Range(-10, -5, false));
  EXPECT_TRUE(filter->testInt64Range(10'234, 20'000, false));
  EXPECT_TRUE(filter->testInt64Range(0, 1, false));
  EXPECT_FALSE(filter->testInt64Range(10, 10, false));
  EXPECT_FALSE(filter->testInt64Range(100, 100, false));
  EXPECT_FALSE(filter->testInt64Range(6, 6, true));

  auto filter_copy = filter->clone();
  EXPECT_FALSE(filter_copy->testInt64(1));
  EXPECT_FALSE(filter_copy->testInt64(10));
  EXPECT_FALSE(filter_copy->testInt64(1000));
  EXPECT_FALSE(filter_copy->testNull());

  EXPECT_TRUE(filter_copy->testInt64(0));
  EXPECT_TRUE(filter_copy->testInt64(102));
  EXPECT_TRUE(filter_copy->testInt64(INT64_MAX));

  EXPECT_TRUE(filter_copy->testInt64Range(5, 50, false));
  EXPECT_TRUE(filter_copy->testInt64Range(11, 11, false));
  EXPECT_FALSE(filter_copy->testInt64Range(10, 10, false));
  EXPECT_FALSE(filter_copy->testInt64Range(6, 6, true));

  auto filter_with_null = filter->clone(true);
  EXPECT_TRUE(filter_with_null->testNull());
  EXPECT_TRUE(filter_with_null->testInt64Range(6, 6, true));

  auto filter_with_null_copy = filter_with_null->clone();
  EXPECT_TRUE(filter_with_null->testNull());

  auto filter_no_more_null = filter_with_null->clone(false);
  EXPECT_FALSE(filter_no_more_null->testNull());
}

TEST(FilterTest, negatedBigintValuesEdgeCases) {
  // cases that should be represented by a non-integer filter
  auto always_true = createNegatedBigintValues({}, true);
  ASSERT_TRUE(dynamic_cast<AlwaysTrue*>(always_true.get()));
  auto not_null = createNegatedBigintValues({}, false);
  ASSERT_TRUE(dynamic_cast<IsNotNull*>(not_null.get()));

  // cases that should trigger creation of a NegatedBigintRange filter
  auto negated_range = createNegatedBigintValues({1, 2, 3, 4, 5, 6, 7}, false);
  ASSERT_TRUE(dynamic_cast<NegatedBigintRange*>(negated_range.get()));
  EXPECT_FALSE(negated_range->testInt64(1));
  EXPECT_FALSE(negated_range->testInt64(3));
  EXPECT_FALSE(negated_range->testInt64(7));
  EXPECT_FALSE(negated_range->testNull());
  EXPECT_TRUE(negated_range->testInt64(0));
  EXPECT_TRUE(negated_range->testInt64(8));
  EXPECT_TRUE(negated_range->testInt64(std::numeric_limits<int64_t>::min()));
  EXPECT_TRUE(negated_range->testInt64(std::numeric_limits<int64_t>::max()));

  std::vector<int64_t> minRangeValues;
  minRangeValues.reserve(10);
  for (int i = 0; i < 10; ++i) {
    minRangeValues.emplace_back(std::numeric_limits<int64_t>::min() + i);
  }
  auto min_range = createNegatedBigintValues(minRangeValues, false);
  ASSERT_TRUE(dynamic_cast<NegatedBigintRange*>(min_range.get()));
  EXPECT_FALSE(min_range->testInt64(std::numeric_limits<int64_t>::min()));
  EXPECT_FALSE(min_range->testInt64(std::numeric_limits<int64_t>::min() + 9));
  EXPECT_FALSE(min_range->testNull());
  EXPECT_TRUE(min_range->testInt64(std::numeric_limits<int64_t>::min() + 10));
  EXPECT_TRUE(min_range->testInt64(0));
  EXPECT_TRUE(min_range->testInt64(std::numeric_limits<int64_t>::max()));

  std::vector<int64_t> maxRangeValues;
  maxRangeValues.reserve(10);
  for (int i = 0; i < 10; ++i) {
    maxRangeValues.emplace_back(std::numeric_limits<int64_t>::max() - i);
  }
  auto max_range = createNegatedBigintValues(maxRangeValues, false);
  ASSERT_TRUE(dynamic_cast<NegatedBigintRange*>(max_range.get()));
  EXPECT_FALSE(max_range->testInt64(std::numeric_limits<int64_t>::max()));
  EXPECT_FALSE(max_range->testInt64(std::numeric_limits<int64_t>::max() - 9));
  EXPECT_FALSE(max_range->testNull());
  EXPECT_TRUE(max_range->testInt64(std::numeric_limits<int64_t>::max() - 10));
  EXPECT_TRUE(max_range->testInt64(0));
  EXPECT_TRUE(max_range->testInt64(std::numeric_limits<int64_t>::min()));

  auto not_equal = createNegatedBigintValues({10}, false);
  ASSERT_TRUE(dynamic_cast<NegatedBigintRange*>(not_equal.get()));
  EXPECT_FALSE(not_equal->testInt64(10));
  EXPECT_FALSE(not_equal->testNull());
  EXPECT_TRUE(not_equal->testInt64(std::numeric_limits<int64_t>::min()));
  EXPECT_TRUE(not_equal->testInt64(std::numeric_limits<int64_t>::max()));
  EXPECT_TRUE(not_equal->testInt64(-1));
  EXPECT_TRUE(not_equal->testInt64(0));
  EXPECT_TRUE(not_equal->testInt64(1));
}

TEST(FilterTest, bigintMultiRange) {
  // x between 1 and 10 or x between 100 and 120
  auto filter = bigintOr(between(1, 10), between(100, 120));
  ASSERT_TRUE(dynamic_cast<BigintMultiRange*>(filter.get()));

  EXPECT_TRUE(filter->testInt64(1));
  EXPECT_TRUE(filter->testInt64(5));
  EXPECT_TRUE(filter->testInt64(10));
  EXPECT_TRUE(filter->testInt64(100));
  EXPECT_TRUE(filter->testInt64(110));
  EXPECT_TRUE(filter->testInt64(120));

  EXPECT_FALSE(filter->testNull());
  EXPECT_FALSE(filter->testInt64(0));
  EXPECT_FALSE(filter->testInt64(50));
  EXPECT_FALSE(filter->testInt64(150));

  EXPECT_TRUE(filter->testInt64Range(5, 15, false));
  EXPECT_TRUE(filter->testInt64Range(5, 15, true));
  EXPECT_TRUE(filter->testInt64Range(105, 115, false));
  EXPECT_TRUE(filter->testInt64Range(105, 115, true));
  EXPECT_FALSE(filter->testInt64Range(15, 45, false));
  EXPECT_FALSE(filter->testInt64Range(15, 45, true));
}

TEST(FilterTest, boolValue) {
  auto boolValueTrue = boolEqual(true);
  EXPECT_TRUE(boolValueTrue->testBool(true));

  EXPECT_FALSE(boolValueTrue->testNull());
  EXPECT_FALSE(boolValueTrue->testBool(false));

  EXPECT_TRUE(boolValueTrue->testInt64Range(0, 1, false));
  EXPECT_TRUE(boolValueTrue->testInt64Range(1, 1, false));
  EXPECT_FALSE(boolValueTrue->testInt64Range(0, 0, false));

  auto boolValueFalse = boolEqual(false);
  EXPECT_TRUE(boolValueFalse->testBool(false));

  EXPECT_FALSE(boolValueFalse->testNull());
  EXPECT_FALSE(boolValueFalse->testBool(true));

  EXPECT_TRUE(boolValueFalse->testInt64Range(0, 1, false));
  EXPECT_FALSE(boolValueFalse->testInt64Range(1, 1, false));
  EXPECT_TRUE(boolValueFalse->testInt64Range(0, 0, false));
}

TEST(FilterTest, doubleRange) {
  auto filter = betweenDouble(1.2, 1.2);
  auto verify = [&](double x) { return filter->testDouble(x); };
  EXPECT_TRUE(filter->testDouble(1.2));

  EXPECT_FALSE(filter->testNull());
  EXPECT_FALSE(filter->testDouble(1.3));
  {
    double n4[] = {1.0, std::nan("nan"), 1.3, 1e200};
    checkSimd(filter.get(), n4, verify);
  }

  filter = lessThanOrEqualDouble(1.2);
  EXPECT_TRUE(filter->testDouble(1.2));
  EXPECT_TRUE(filter->testDouble(1.1));

  EXPECT_FALSE(filter->testNull());
  EXPECT_FALSE(filter->testDouble(1.3));
  {
    double n4[] = {-1e100, std::nan("nan"), 1.3, 1e200};
    checkSimd(filter.get(), n4, verify);
  }

  filter = greaterThanDouble(1.2);
  EXPECT_TRUE(filter->testDouble(1.3));
  EXPECT_TRUE(filter->testDouble(5.6));

  EXPECT_FALSE(filter->testNull());
  EXPECT_FALSE(filter->testDouble(1.2));
  EXPECT_FALSE(filter->testDouble(-19.267));
  EXPECT_TRUE(filter->testDouble(std::nanf("nan1")));
  EXPECT_TRUE(filter->testDouble(std::nanf("nan2")));
  {
    double n4[] = {-1e100, std::nan("nan"), 1.3, 1e200};
    checkSimd(filter.get(), n4, verify);
  }

  filter = betweenDouble(1.2, 3.4);
  EXPECT_TRUE(filter->testDouble(1.2));
  EXPECT_TRUE(filter->testDouble(1.5));
  EXPECT_TRUE(filter->testDouble(3.4));

  EXPECT_FALSE(filter->testNull());
  EXPECT_FALSE(filter->testDouble(-0.3));
  EXPECT_FALSE(filter->testDouble(55.6));
  EXPECT_FALSE(filter->testDouble(NAN));

  {
    double n4[] = {3.4, 1.3, 1.1, 1e200};
    checkSimd(filter.get(), n4, verify);
  }

  EXPECT_THROW(betweenDouble(NAN, NAN), VeloxRuntimeError)
      << "able to create a DoubleRange with NaN";

  // A filter that has upper value set but really is unbounded.
  filter = std::make_unique<common::DoubleRange>(
      3, false, false, 0, true, false, true);
  EXPECT_TRUE(filter->testDoubleRange(1, 100, false));
  EXPECT_FALSE(filter->testDoubleRange(0, 1, false));
  EXPECT_TRUE(filter->testDoubleRange(0, 1, true));
  EXPECT_FALSE(filter->testDouble(1));
  EXPECT_TRUE(filter->testDouble(3));
  EXPECT_TRUE(filter->testDouble(100));
}

TEST(FilterTest, floatRange) {
  auto filter = betweenFloat(1.2, 1.2);
  auto verify = [&](float x) { return filter->testFloat(x); };
  EXPECT_TRUE(filter->testFloat(1.2f));

  EXPECT_FALSE(filter->testNull());
  EXPECT_FALSE(filter->testFloat(1.1f));
  {
    float n8[] = {1.0, std::nanf("nan"), 1.3, 1e20, -1e20, 0, 0, 0};
    checkSimd(filter.get(), n8, verify);
  }

  filter = lessThanFloat(1.2);
  EXPECT_TRUE(filter->testFloat(1.1f));

  EXPECT_FALSE(filter->testNull());
  EXPECT_FALSE(filter->testFloat(1.2f));
  EXPECT_FALSE(filter->testFloat(15.632f));
  {
    float n8[] = {1.0, std::nanf("nan"), 1.3, 1e20, -1e20, 0, 1.1, 1.2};
    checkSimd(filter.get(), n8, verify);
  }

  filter = betweenFloat(1.2, 3.4);
  EXPECT_TRUE(filter->testFloat(1.2f));
  EXPECT_TRUE(filter->testFloat(2.3f));
  EXPECT_TRUE(filter->testFloat(3.4f));

  EXPECT_FALSE(filter->testNull());
  EXPECT_FALSE(filter->testFloat(1.1f));
  EXPECT_FALSE(filter->testFloat(15.632f));
  EXPECT_FALSE(filter->testFloat(std::nanf("NAN")));
  {
    float n8[] = {1.0, std::nanf("nan"), 3.4, 3.1, -1e20, 0, 1.1, 1.2};
    checkSimd(filter.get(), n8, verify);
  }

  filter = greaterThanFloat(1.2);
  EXPECT_FALSE(filter->testFloat(1.1f));

  EXPECT_FALSE(filter->testNull());
  EXPECT_FALSE(filter->testFloat(1.2f));
  EXPECT_TRUE(filter->testFloat(15.632f));
  EXPECT_TRUE(filter->testFloat(std::nanf("nan1")));
  EXPECT_TRUE(filter->testFloat(std::nanf("nan2")));
  {
    float n8[] = {1.0, std::nanf("nan"), 1.3, 1e20, -1e20, 0, 1.1, 1.2};
    checkSimd(filter.get(), n8, verify);
  }

  EXPECT_THROW(
      betweenFloat(std::nanf("NAN"), std::nanf("NAN")), VeloxRuntimeError)
      << "able to create a FloatRange with NaN";

  // A filter that has upper value set but really is unbounded.
  filter = std::make_unique<common::FloatRange>(
      3, false, false, 0, true, false, true);
  EXPECT_TRUE(filter->testDoubleRange(1, 100, false));
  EXPECT_FALSE(filter->testDoubleRange(0, 1, false));
  EXPECT_TRUE(filter->testDoubleRange(0, 1, true));
  EXPECT_FALSE(filter->testFloat(1));
  EXPECT_TRUE(filter->testFloat(3));
  EXPECT_TRUE(filter->testFloat(100));
}

TEST(FilterTest, bytesRange) {
  {
    auto filter = equal("abc");
    EXPECT_TRUE(filter->testBytes("abc", 3));
    EXPECT_FALSE(filter->testBytes("acb", 3));
    EXPECT_TRUE(filter->testLength(3));
    // The bit for lane 2 should be set.
    int32_t lens[] = {0, 1, 3, 0, 4, 10, 11, 12};
    EXPECT_EQ(
        4, simd::toBitMask(filter->testLengths(xsimd::load_unaligned(lens))));

    EXPECT_FALSE(filter->testNull());
    EXPECT_FALSE(filter->testBytes("apple", 5));
    EXPECT_FALSE(filter->testBytes(nullptr, 0));
    EXPECT_FALSE(filter->testLength(4));

    EXPECT_TRUE(filter->testBytesRange("abc", "abc", false));
    EXPECT_TRUE(filter->testBytesRange("a", "z", false));
    EXPECT_TRUE(filter->testBytesRange("aaaaa", "bbbb", false));
    EXPECT_FALSE(filter->testBytesRange("apple", "banana", false));
    EXPECT_FALSE(filter->testBytesRange("orange", "plum", false));

    EXPECT_FALSE(filter->testBytesRange(std::nullopt, "a", false));
    EXPECT_TRUE(filter->testBytesRange(std::nullopt, "abc", false));
    EXPECT_TRUE(filter->testBytesRange(std::nullopt, "banana", false));

    EXPECT_FALSE(filter->testBytesRange("banana", std::nullopt, false));
    EXPECT_TRUE(filter->testBytesRange("abc", std::nullopt, false));
    EXPECT_TRUE(filter->testBytesRange("a", std::nullopt, false));

    // = ''
    filter = equal("");
    EXPECT_TRUE(filter->testBytes(nullptr, 0));
    EXPECT_FALSE(filter->testBytes("abc", 3));
  }

  char const* theBestOfTimes =
      "It was the best of times, it was the worst of times, it was the age of wisdom, it was the age of foolishness, it was the epoch of belief, it was the epoch of incredulity,...";
  auto filter = lessThanOrEqual(theBestOfTimes);
  EXPECT_TRUE(filter->testBytes(theBestOfTimes, std::strlen(theBestOfTimes)));
  EXPECT_TRUE(filter->testBytes(theBestOfTimes, 5));
  EXPECT_TRUE(filter->testBytes(theBestOfTimes, 50));
  EXPECT_TRUE(filter->testBytes(theBestOfTimes, 100));
  // testLength is true of all lengths for a range filter.
  EXPECT_TRUE(filter->testLength(1));
  EXPECT_TRUE(filter->testLength(1000));

  EXPECT_FALSE(filter->testNull());
  EXPECT_FALSE(filter->testBytes("Zzz", 3));
  EXPECT_FALSE(filter->testBytes("It was the best of times, zzz", 30));

  EXPECT_TRUE(filter->testBytesRange("Apple", "banana", false));
  EXPECT_FALSE(filter->testBytesRange("Pear", "Plum", false));
  EXPECT_FALSE(filter->testBytesRange("apple", "banana", false));

  filter = greaterThanOrEqual("abc");
  EXPECT_TRUE(filter->testBytes("abc", 3));
  EXPECT_TRUE(filter->testBytes("ad", 2));
  EXPECT_TRUE(filter->testBytes("apple", 5));
  EXPECT_TRUE(filter->testBytes("banana", 6));

  EXPECT_FALSE(filter->testNull());
  EXPECT_FALSE(filter->testBytes("ab", 2));
  EXPECT_FALSE(filter->testBytes("_abc", 4));

  filter = between("apple", "banana");
  EXPECT_TRUE(filter->testBytes("apple", 5));
  EXPECT_TRUE(filter->testBytes("banana", 6));
  EXPECT_TRUE(filter->testBytes("avocado", 7));

  EXPECT_FALSE(filter->testNull());
  EXPECT_FALSE(filter->testBytes("camel", 5));
  EXPECT_FALSE(filter->testBytes("_abc", 4));

  filter = std::make_unique<BytesRange>(
      "apple", false, true, "banana", false, false, false);
  EXPECT_TRUE(filter->testBytes("banana", 6));
  EXPECT_TRUE(filter->testBytes("avocado", 7));

  EXPECT_FALSE(filter->testNull());
  EXPECT_FALSE(filter->testBytes("apple", 5));
  EXPECT_FALSE(filter->testBytes("camel", 5));
  EXPECT_FALSE(filter->testBytes("_abc", 4));

  filter = std::make_unique<BytesRange>(
      "apple", false, true, "banana", false, true, false);
  EXPECT_TRUE(filter->testBytes("avocado", 7));

  EXPECT_FALSE(filter->testNull());
  EXPECT_FALSE(filter->testBytes("apple", 5));
  EXPECT_FALSE(filter->testBytes("banana", 6));
  EXPECT_FALSE(filter->testBytes("camel", 5));
  EXPECT_FALSE(filter->testBytes("_abc", 4));

  // < b
  filter = lessThan("b");
  EXPECT_TRUE(filter->testBytes("a", 1));
  EXPECT_FALSE(filter->testBytes("b", 1));
  EXPECT_FALSE(filter->testBytes("c", 1));
  EXPECT_TRUE(filter->testBytes(nullptr, 0));
  EXPECT_FALSE(filter->testBytesRange("b", "c", false));

  // <= b
  filter = lessThanOrEqual("b");
  EXPECT_TRUE(filter->testBytes("a", 1));
  EXPECT_TRUE(filter->testBytes("b", 1));
  EXPECT_FALSE(filter->testBytes("c", 1));
  EXPECT_TRUE(filter->testBytes(nullptr, 0));
  EXPECT_TRUE(filter->testBytesRange("b", "c", false));

  // >= b
  filter = greaterThanOrEqual("b");
  EXPECT_FALSE(filter->testBytes("a", 1));
  EXPECT_TRUE(filter->testBytes("b", 1));
  EXPECT_TRUE(filter->testBytes("c", 1));
  EXPECT_FALSE(filter->testBytes(nullptr, 0));
  EXPECT_TRUE(filter->testBytesRange("a", "b", false));

  // > b
  filter = greaterThan("b");
  EXPECT_FALSE(filter->testBytes("a", 1));
  EXPECT_FALSE(filter->testBytes("b", 1));
  EXPECT_TRUE(filter->testBytes("c", 1));
  EXPECT_FALSE(filter->testBytes(nullptr, 0));
  EXPECT_FALSE(filter->testBytesRange("a", "b", false));

  // < ''
  filter = lessThan("");
  EXPECT_FALSE(filter->testBytes(nullptr, 0));
  EXPECT_FALSE(filter->testBytes("abc", 3));

  // <= ''
  filter = lessThanOrEqual("");
  EXPECT_TRUE(filter->testBytes(nullptr, 0));
  EXPECT_FALSE(filter->testBytes("abc", 3));

  // > ''
  filter = greaterThan("");
  EXPECT_FALSE(filter->testBytes(nullptr, 0));
  EXPECT_TRUE(filter->testBytes("abc", 3));

  // >= ''
  filter = greaterThanOrEqual("");
  EXPECT_TRUE(filter->testBytes(nullptr, 0));
  EXPECT_TRUE(filter->testBytes("abc", 3));
}

TEST(FilterTest, negatedBytesRange) {
  auto filter = notBetween("a", "c");

  EXPECT_TRUE(filter->testBytes("A", 1));
  EXPECT_TRUE(filter->testBytes(nullptr, 0));
  EXPECT_TRUE(filter->testBytes("ca", 2));
  EXPECT_TRUE(filter->testBytes("z", 1));

  EXPECT_FALSE(filter->testBytes("a", 1));
  EXPECT_FALSE(filter->testBytes("apple", 5));
  EXPECT_FALSE(filter->testBytes("c", 1));
  EXPECT_FALSE(filter->testNull());

  EXPECT_TRUE(filter->testLength(1));
  EXPECT_TRUE(filter->testLength(3));
  EXPECT_TRUE(filter->testLength(5));
  EXPECT_TRUE(filter->testLength(100));

  EXPECT_TRUE(filter->testBytesRange(std::nullopt, "b", false));
  EXPECT_TRUE(filter->testBytesRange("a", std::nullopt, false));
  EXPECT_TRUE(filter->testBytesRange("a", "ca", false));
  EXPECT_TRUE(filter->testBytesRange("", "c", false));

  EXPECT_FALSE(filter->testBytesRange("a", "c", false));
  EXPECT_FALSE(filter->testBytesRange("ab", "bzzzzzzz", false));

  EXPECT_FALSE(filter->isSingleValue());
  EXPECT_EQ("a", filter->lower());
  EXPECT_EQ("c", filter->upper());
  EXPECT_FALSE(filter->isLowerUnbounded());
  EXPECT_FALSE(filter->isUpperUnbounded());
  EXPECT_FALSE(filter->isLowerExclusive());
  EXPECT_FALSE(filter->isUpperExclusive());

  filter = notBetweenExclusive("b", "d");
  EXPECT_TRUE(filter->testBytes("b", 1));
  EXPECT_TRUE(filter->testBytes("d", 1));

  EXPECT_TRUE(filter->testBytesRange("b", "c", false));
  EXPECT_TRUE(filter->testBytesRange("c", "d", false));
  EXPECT_FALSE(filter->testBytesRange("bb", "cc", true));

  EXPECT_FALSE(filter->isSingleValue());
  EXPECT_EQ("b", filter->lower());
  EXPECT_EQ("d", filter->upper());
  EXPECT_FALSE(filter->isLowerUnbounded());
  EXPECT_FALSE(filter->isUpperUnbounded());
  EXPECT_TRUE(filter->isLowerExclusive());
  EXPECT_TRUE(filter->isUpperExclusive());

  auto filter_with_null = filter->clone(true);
  EXPECT_TRUE(filter_with_null->testBytesRange("bb", "cc", true));
  EXPECT_TRUE(filter_with_null->testNull());

  auto filter_with_null_copy = filter_with_null->clone();
  EXPECT_TRUE(filter_with_null_copy->testBytesRange("bb", "cc", true));
  EXPECT_TRUE(filter_with_null_copy->testNull());
}

TEST(FilterTest, bytesValues) {
  // The filter has values of size on either side of 8 bytes.
  std::vector<std::string> values({"Igne", "natura", "renovitur", "integra."});
  auto filter = in(values);
  EXPECT_TRUE(filter->testBytes("Igne", 4));
  EXPECT_TRUE(filter->testBytes("natura", 6));
  EXPECT_TRUE(filter->testBytes("natural", 6));
  EXPECT_TRUE(filter->testBytes("renovitur", 9));
  EXPECT_TRUE(filter->testBytes("integra.", 8));

  EXPECT_FALSE(filter->testNull());
  EXPECT_FALSE(filter->testBytes("natura", 5));
  EXPECT_FALSE(filter->testBytes("apple", 5));

  EXPECT_TRUE(filter->testLength(4));
  EXPECT_TRUE(filter->testLength(6));
  EXPECT_TRUE(filter->testLength(8));
  EXPECT_TRUE(filter->testLength(9));

  EXPECT_FALSE(filter->testLength(1));
  EXPECT_FALSE(filter->testLength(5));
  EXPECT_FALSE(filter->testLength(125));

  EXPECT_TRUE(filter->testBytesRange("natura", "naturel", false));
  EXPECT_TRUE(filter->testBytesRange("igloo", "ocean", false));
  EXPECT_FALSE(filter->testBytesRange("igloo", "igloo", false));
  EXPECT_FALSE(filter->testBytesRange("Apple", "Banana", false));
  EXPECT_FALSE(filter->testBytesRange("sun", "water", false));

  EXPECT_TRUE(filter->testBytesRange("natura", std::nullopt, false));
  EXPECT_TRUE(filter->testBytesRange("igloo", std::nullopt, false));
  EXPECT_FALSE(filter->testBytesRange("sun", std::nullopt, false));

  EXPECT_TRUE(filter->testBytesRange(std::nullopt, "Igne", false));
  EXPECT_TRUE(filter->testBytesRange(std::nullopt, "ocean", false));
  EXPECT_FALSE(filter->testBytesRange(std::nullopt, "Banana", false));
}

TEST(FilterTest, negatedBytesValues) {
  // create a filter
  std::vector<std::string> values(
      {"fifteen", "two", "ten", "extended", "vocabulary"});
  auto filter = notIn(values);

  EXPECT_TRUE(filter->testBytes("hello", 5));
  EXPECT_TRUE(filter->testBytes("tent", 4));
  EXPECT_TRUE(filter->testBytes("", 0));
  EXPECT_TRUE(filter->testBytes("ten", 1)); // short-circuit accept by length

  EXPECT_FALSE(filter->testBytes("extended", 8));
  EXPECT_FALSE(filter->testBytes("vocabulary", 10));
  EXPECT_FALSE(filter->testBytes("two", 3));
  EXPECT_FALSE(filter->testNull());

  EXPECT_TRUE(filter->testLength(0));
  EXPECT_TRUE(filter->testLength(1));
  EXPECT_TRUE(filter->testLength(3));
  EXPECT_TRUE(filter->testLength(99));
  EXPECT_TRUE(filter->testLength(std::numeric_limits<int32_t>::max()));

  EXPECT_TRUE(filter->testBytesRange("a", "b", false));
  EXPECT_TRUE(filter->testBytesRange("hello", "helloa", false));
  EXPECT_TRUE(filter->testBytesRange("hello", "hello", false));
  EXPECT_TRUE(filter->testBytesRange("b", "a", false));
  EXPECT_TRUE(filter->testBytesRange("zzz", std::nullopt, false));
  EXPECT_TRUE(filter->testBytesRange("", std::nullopt, false));
  EXPECT_TRUE(filter->testBytesRange(std::nullopt, "a", false));
  EXPECT_TRUE(filter->testBytesRange(std::nullopt, "zzzzz", false));
  EXPECT_TRUE(filter->testBytesRange("ten", "two", false));

  EXPECT_FALSE(filter->testBytesRange("two", "two", false));
  EXPECT_FALSE(filter->testBytesRange("two", "two", true));

  auto filter_copy = std::make_unique<NegatedBytesValues>(*filter, true);
  EXPECT_TRUE(filter_copy->testNull());
  EXPECT_TRUE(filter_copy->testBytes("hello", 5));
  EXPECT_TRUE(filter_copy->testBytes("tent", 4));
  EXPECT_TRUE(filter_copy->testLength(2));
  EXPECT_TRUE(filter_copy->testLength(6));
  EXPECT_TRUE(filter_copy->testBytesRange("hello", "hello", false));
  EXPECT_TRUE(filter_copy->testBytesRange("two", "two", true));

  EXPECT_FALSE(filter_copy->testBytes("extended", 8));
  EXPECT_FALSE(filter_copy->testBytesRange("ten", "ten", false));

  auto filter_with_nulls = filter->clone(true);
  EXPECT_TRUE(filter_with_nulls->testNull());
  EXPECT_TRUE(filter_with_nulls->testBytesRange("fifteen", "fifteen", true));
  EXPECT_FALSE(filter_with_nulls->testBytes("fifteen", 7));
  EXPECT_FALSE(filter_with_nulls->testBytesRange("fifteen", "fifteen", false));

  auto filter_nulls_copy = filter_with_nulls->clone();
  EXPECT_TRUE(filter_nulls_copy->testNull());
  EXPECT_TRUE(filter_nulls_copy->testBytesRange("fifteen", "fifteen", true));
  EXPECT_FALSE(filter_nulls_copy->testBytes("fifteen", 7));
  EXPECT_FALSE(filter_nulls_copy->testBytesRange("fifteen", "fifteen", false));

  auto filter_no_nulls = filter_nulls_copy->clone(false);
  EXPECT_FALSE(filter_no_nulls->testNull());
}

TEST(FilterTest, multiRange) {
  auto filter = orFilter(between("abc", "abc"), greaterThanOrEqual("dragon"));

  EXPECT_TRUE(filter->testBytes("abc", 3));
  EXPECT_TRUE(filter->testBytes("dragon", 6));
  EXPECT_TRUE(filter->testBytes("dragonfly", 9));
  EXPECT_TRUE(filter->testBytes("drought", 7));

  EXPECT_FALSE(filter->testNull());
  EXPECT_FALSE(filter->testBytes("apple", 5));

  EXPECT_TRUE(filter->testBytesRange("abc", "abc", false));
  EXPECT_TRUE(filter->testBytesRange("apple", "pear", false));
  EXPECT_FALSE(filter->testBytesRange("apple", "banana", false));
  EXPECT_FALSE(filter->testBytesRange("aaa", "aa123", false));

  filter = orFilter(lessThanDouble(1.2), greaterThanDouble(1.2));

  EXPECT_TRUE(filter->testDouble(1.1));
  EXPECT_TRUE(filter->testDouble(1.3));

  EXPECT_FALSE(filter->testNull());
  EXPECT_TRUE(filter->testDouble(std::nan("nan")));
  EXPECT_FALSE(filter->testDouble(1.2));

  filter = orFilter(lessThanFloat(1.2), greaterThanFloat(1.2));

  EXPECT_FALSE(filter->testNull());
  EXPECT_TRUE(filter->testFloat(1.1f));
  EXPECT_FALSE(filter->testFloat(1.2f));
  EXPECT_TRUE(filter->testFloat(1.3f));
  EXPECT_TRUE(filter->testFloat(std::nanf("nan")));

  // != ''
  filter = orFilter(lessThan(""), greaterThan(""));
  EXPECT_FALSE(filter->testBytes(nullptr, 0));
  EXPECT_TRUE(filter->testBytes("abc", 3));
}

TEST(FilterTest, multiRangeWithNaNs) {
  // x <> 1.2 with nanAllowed true
  auto filter = orFilter(lessThanFloat(1.2), greaterThanFloat(1.2), false);
  EXPECT_TRUE(filter->testFloat(std::nanf("nan")));
  EXPECT_FALSE(filter->testFloat(1.2f));
  EXPECT_TRUE(filter->testFloat(1.1f));

  filter = orFilter(lessThanDouble(1.2), greaterThanDouble(1.2), false);
  EXPECT_TRUE(filter->testDouble(std::nan("nan")));
  EXPECT_FALSE(filter->testDouble(1.2));
  EXPECT_TRUE(filter->testDouble(1.1));

  // x <> 1.2 with nanAllowed false
  filter = orFilter(lessThanFloat(1.2), greaterThanFloat(1.2));
  EXPECT_TRUE(filter->testFloat(std::nanf("nan")));
  EXPECT_TRUE(filter->testFloat(1.0f));

  filter = orFilter(lessThanDouble(1.2), greaterThanDouble(1.2));
  EXPECT_TRUE(filter->testDouble(std::nan("nan")));
  EXPECT_TRUE(filter->testDouble(1.4));

  // x NOT IN (1.2, 1.3) with nanAllowed true
  filter = orFilter(lessThanFloat(1.2), greaterThanFloat(1.3), false);
  EXPECT_TRUE(filter->testFloat(std::nanf("nan")));
  EXPECT_FALSE(filter->testFloat(1.2f));
  EXPECT_FALSE(filter->testFloat(1.3f));
  EXPECT_TRUE(filter->testFloat(1.4f));
  EXPECT_TRUE(filter->testFloat(1.1f));

  filter = orFilter(lessThanDouble(1.2), greaterThanDouble(1.3), false);
  EXPECT_TRUE(filter->testDouble(std::nan("nan")));
  EXPECT_FALSE(filter->testDouble(1.2));
  EXPECT_FALSE(filter->testDouble(1.3));
  EXPECT_TRUE(filter->testDouble(1.4));
  EXPECT_TRUE(filter->testDouble(1.1));
}

TEST(FilterTest, createBigintValues) {
  // Small number of values from a very large range.
  {
    std::vector<int64_t> values = {
        std::numeric_limits<int64_t>::max() - 1'000,
        std::numeric_limits<int64_t>::min() + 1'000,
        0,
        123};
    auto filter = createBigintValues(values, true);
    ASSERT_TRUE(dynamic_cast<BigintValuesUsingHashTable*>(filter.get()))
        << filter->toString();
    for (auto v : values) {
      ASSERT_TRUE(filter->testInt64(v));
    }
    ASSERT_FALSE(filter->testInt64(-5));
    ASSERT_FALSE(filter->testInt64(12345));
    ASSERT_TRUE(filter->testNull());
  }

  // Small number of values from a small range.
  {
    std::vector<int64_t> values = {0, 123, -7, 56};
    auto filter = createBigintValues(values, true);
    ASSERT_TRUE(dynamic_cast<BigintValuesUsingBitmask*>(filter.get()))
        << filter->toString();
    for (auto v : values) {
      ASSERT_TRUE(filter->testInt64(v));
    }
    ASSERT_FALSE(filter->testInt64(-5));
    ASSERT_FALSE(filter->testInt64(12345));
    ASSERT_TRUE(filter->testNull());
  }

  // Dense sequence of values without gaps.
  {
    std::vector<int64_t> values(100);
    std::iota(values.begin(), values.end(), 5);
    auto filter = createBigintValues(values, false);
    ASSERT_TRUE(dynamic_cast<BigintRange*>(filter.get())) << filter->toString();
    for (int i = 5; i < 105; i++) {
      ASSERT_TRUE(filter->testInt64(i));
    }
    ASSERT_FALSE(filter->testInt64(4));
    ASSERT_FALSE(filter->testInt64(106));
    ASSERT_FALSE(filter->testNull());
  }

  // Single value.
  {
    std::vector<int64_t> values = {37};
    auto filter = createBigintValues(values, false);
    ASSERT_TRUE(dynamic_cast<BigintRange*>(filter.get())) << filter->toString();
    for (int i = -100; i <= 100; i++) {
      if (i != 37) {
        ASSERT_FALSE(filter->testInt64(i));
      }
    }
    ASSERT_TRUE(filter->testInt64(37));
    ASSERT_FALSE(filter->testNull());
  }
}

namespace {

void addUntypedFilters(std::vector<std::unique_ptr<Filter>>& filters) {
  filters.push_back(std::make_unique<AlwaysFalse>());
  filters.push_back(std::make_unique<AlwaysTrue>());
  filters.push_back(std::make_unique<IsNull>());
  filters.push_back(std::make_unique<IsNotNull>());
}

void testMergeWithUntyped(Filter* left, Filter* right) {
  auto merged = left->mergeWith(right);

  // Null.
  ASSERT_EQ(merged->testNull(), left->testNull() && right->testNull());

  // Not null.
  ASSERT_EQ(merged->testNonNull(), left->testNonNull() && right->testNonNull());

  // Integer value.
  ASSERT_EQ(
      merged->testInt64(123), left->testInt64(123) && right->testInt64(123));
}

void testMergeWithBool(Filter* left, Filter* right) {
  auto merged = left->mergeWith(right);
  ASSERT_EQ(merged->testNull(), left->testNull() && right->testNull());
  ASSERT_EQ(
      merged->testBool(true), left->testBool(true) && right->testBool(true));
  ASSERT_EQ(
      merged->testBool(false), left->testBool(false) && right->testBool(false));
}

void testMergeWithBigint(Filter* left, Filter* right) {
  auto merged = left->mergeWith(right);

  ASSERT_EQ(merged->testNull(), left->testNull() && right->testNull())
      << "left: " << left->toString() << ", right: " << right->toString()
      << ", merged: " << merged->toString();

  for (int64_t i = -1'000; i <= 1'000; i++) {
    ASSERT_EQ(merged->testInt64(i), left->testInt64(i) && right->testInt64(i))
        << "at " << i << ", left: " << left->toString()
        << ", right: " << right->toString()
        << ", merged: " << merged->toString();
  }
  // check empty marker
  ASSERT_EQ(
      merged->testInt64(0xdeadbeefbadefeedL),
      left->testInt64(0xdeadbeefbadefeedL) &&
          right->testInt64(0xdeadbeefbadefeedL));
}

void testMergeWithDouble(Filter* left, Filter* right) {
  auto merged = left->mergeWith(right);
  ASSERT_EQ(merged->testNull(), left->testNull() && right->testNull());
  for (int64_t i = -10; i <= 10; i++) {
    double d = i * 0.1;
    ASSERT_EQ(
        merged->testDouble(d), left->testDouble(d) && right->testDouble(d));
  }
}

void testMergeWithFloat(Filter* left, Filter* right) {
  auto merged = left->mergeWith(right);
  ASSERT_EQ(merged->testNull(), left->testNull() && right->testNull());
  for (int64_t i = -10; i <= 10; i++) {
    float f = i * 0.1;
    ASSERT_EQ(merged->testFloat(f), left->testFloat(f) && right->testFloat(f));
  }
}

void testMergeWithBytes(Filter* left, Filter* right) {
  auto merged = left->mergeWith(right);

  ASSERT_EQ(merged->testNull(), left->testNull() && right->testNull())
      << "left: " << left->toString() << ", right: " << right->toString()
      << ", merged: " << merged->toString();

  std::vector<std::string> testValues = {
      "",
      "a",
      "b",
      "c",
      "d",
      "e",
      "f",
      "g",
      "h",
      "i",
      "j",
      "k",
      "l",
      "m",
      "n",
      "o",
      "p",
      "q",
      "r",
      "s",
      "t",
      "u",
      "v",
      "w",
      "x",
      "y",
      "z",
      "abca",
      "abcb",
      "abcc",
      "abcd",
      "A",
      "B",
      "C",
      "D",
      "AB",
      "AC",
      "AD",
      "0",
      "1",
      "2",
      "11",
      "22",
      "12345",
      "AbcDedFghIJkl",
      "!@3456^&*()",
      "!",
      "#",
      "^&",
      "-=",
      " ",
      "[]",
      "|",
      "?",
      "?!",
      "!?!",
      "+",
      "/",
      "<",
      ">",
      "=",
      "//<<>>"};

  for (const auto& test : testValues) {
    ASSERT_EQ(
        merged->testBytes(test.data(), test.size()),
        left->testBytes(test.data(), test.size()) &&
            right->testBytes(test.data(), test.size()))
        << test << " " << "left: " << left->toString()
        << ", right: " << right->toString()
        << ", merged: " << merged->toString();
  }
}
} // namespace

TEST(FilterTest, mergeWithUntyped) {
  std::vector<std::unique_ptr<Filter>> filters;
  addUntypedFilters(filters);

  for (const auto& left : filters) {
    for (const auto& right : filters) {
      testMergeWithUntyped(left.get(), right.get());
    }
  }
}

TEST(FilterTest, mergeWithBool) {
  std::vector<std::unique_ptr<Filter>> filters;
  addUntypedFilters(filters);
  filters.push_back(boolEqual(true, false));
  filters.push_back(boolEqual(true, true));
  filters.push_back(boolEqual(false, false));
  filters.push_back(boolEqual(false, true));

  for (const auto& left : filters) {
    for (const auto& right : filters) {
      testMergeWithBool(left.get(), right.get());
    }
  }
}

TEST(FilterTest, mergeWithBigint) {
  std::vector<std::unique_ptr<Filter>> filters;
  addUntypedFilters(filters);
  // Equality.
  filters.push_back(equal(123));
  filters.push_back(equal(123, true));

  // Between.
  filters.push_back(between(-7, 13));
  filters.push_back(between(-7, 13, true));
  filters.push_back(between(150, 500));
  filters.push_back(between(150, 500, true));

  // Inequality.
  filters.push_back(notEqual(123));
  filters.push_back(notEqual(123, true));
  filters.push_back(notEqual(300));
  filters.push_back(notEqual(300, true));
  filters.push_back(notEqual(2));
  filters.push_back(notEqual(3));

  // Not between.
  filters.push_back(notBetween(150, 500));
  filters.push_back(notBetween(150, 500, true));
  filters.push_back(notBetween(0, 100));
  filters.push_back(notBetween(0, 100, true));
  filters.push_back(notBetween(0, 200));
  filters.push_back(notBetween(400, 600));
  filters.push_back(notBetween(0, 600));
  filters.push_back(notBetween(1000, 10'134));
  filters.push_back(notBetween(200, 300));

  // IN-list.
  filters.push_back(in({1, 2, 3, 67'000'000'000, 134}));
  filters.push_back(in({1, 2, 3, 67'000'000'000, 134}, true));
  filters.push_back(in({-7, -6, -5, -4, -3, -2}));
  filters.push_back(in({-7, -6, -5, -4, -3, -2}, true));
  filters.push_back(in({1, 2, 3, 67, 10'134}));
  filters.push_back(in({1, 2, 3, 67, 10'134}, true));
  int64_t empty = 0xdeadbeefbadefeedL;
  filters.push_back(in({1, 5, 210, empty}, false));
  filters.push_back(in({1, 5, 210, empty}, true));
  filters.push_back(in({empty - 10, empty, empty + 5}, false));
  filters.push_back(in({empty - 10, empty, empty + 5}, true));
  filters.push_back(
      in({303624755802858,
          384136357751697,
          423368943828438,
          476859918288764,
          515956188024717,
          530476219784376,
          540277308863418,
          541942388637759,
          547188151816561,
          594891360374231,
          2959637400868637,
          3721199441487026,
          5114096828649969,
          5740659369327042,
          7458347650862984,
          7772595162800109,
          9047066205367949,
          9499197740094903,
          10164541576263747},
         false));
  filters.push_back(
      in({366156343044031,   419600327914330,   422718254267138,
          424415250736060,   432660186580905,   433507116523339,
          435212426352282,   438781105992603,   462668840270469,
          469721892897704,   475424508904228,   483961588133744,
          489952690871193,   490100434182734,   490819974117919,
          497522143079784,   498117470059295,   507069815819932,
          510252055500200,   511649005360431,   528948513624967,
          530007393190773,   532093806643372,   532858776337696,
          533698686153076,   534941456359069,   534952966357439,
          535328579401504,   536514619192305,   538518528865308,
          539629822309977,   541635018758516,   542645398681605,
          546538218258352,   549075251184136,   552034220783356,
          555830763849991,   556128663698410,   557441673901702,
          557518250566058,   558506873800117,   560841773258853,
          564440396502105,   564619926218582,   565301696087188,
          566576865977204,   567327966039757,   571300905632024,
          571507642484409,   576283914994222,   583838294159962,
          584905160854028,   585126200832284,   585144667234794,
          585512340796065,   589020370310849,   590619753637829,
          590968883472318,   602908642207125,   607722928272194,
          630688649629213,   885942503719703,   901036228842690,
          962070099101869,   969139801894639,   969995364959528,
          973627137921970,   1002311311705971,  1038877281341469,
          1065903171959919,  1087370529605788,  1098287305028396,
          1101645231614814,  1102068331524993,  1105447177903198,
          1133861121777932,  1276407910222050,  1287904945548777,
          1288419322354242,  1291701008519687,  1316499312639548,
          1673720443174607,  1718049419052929,  1764589184342287,
          1956371744842865,  1957088988143983,  2294783974193603,
          2310611259279723,  2489988147857778,  2506680886188504,
          2778644458941619,  2864989937002641,  3330532020422880,
          3379144482229182,  3531656983798849,  3684799191810397,
          3767646070216820,  3780026845640994,  3866482590263275,
          3908237629460878,  3929286414014934,  3997016003866875,
          6662924023746038,  8514927431889941,  8612971988750230,
          8636020789790871,  8719877121393549,  8725132074239741,
          8780651958686524,  8787110144711385,  8787330658025567,
          8819046344852395,  8924909114261419,  8975532075831271,
          9259979694035612,  9315469178463099,  9337669522923600,
          9568982303132896,  9577455068986597,  9581431555203631,
          9829903477024362,  10109338878839298, 10111913146675745,
          10116454049060631, 10124906929770020, 10134576737527714,
          10159848794251706, 10160080836411106, 10160121877061863,
          10160278547826680, 10160634373033302, 10160634378133302,
          10160634384343302, 10160690892401937, 10160813905552695,
          10160947889699506, 10160952984619779, 10161103697043337,
          10161366446441677, 10161764887950446, 10161987568435865,
          10161990333441343, 10162023187830630, 10162078292985953,
          10162218597897184, 10162437068533464, 10162437274543464,
          10162778721371988, 10162785900361133, 10162858926244783,
          10163123124869467, 10163146005786988, 10163215994438487,
          10163381914257985, 10164541576263747, 10164917662438438,
          10169186680190153, 10169728530660634, 10170078224595191,
          10222519309531166, 10222619264790035, 10226169282240298,
          10226533527356871, 10226809478652237, 10228886958906475,
          10229587253999927, 10230037972188280, 10230384967105126,
          10230604232962437, 10230895412882189, 10231056553451466,
          10231187746449346, 10231194618102996, 10231349384532060,
          10231749348709149, 10231816240283162, 10232659299412293,
          10232936398942338, 10233350393082617, 10233775873439095,
          10233821539425403, 10234016712428289, 10234070019035259,
          10234328283366746, 10234421657513549, 10234695927556443,
          10236169460675790, 10236184353810192, 10236719696391339,
          10236734971453206, 10236817060225653, 28113146188270673,
          28349528491304705},
         false));

  // NOT IN-list.
  filters.push_back(notIn({1, 2, 3, 67'000'000'000, 134}));
  filters.push_back(notIn({1, 2, 3, 67'000'000'000, 134}, true));
  filters.push_back(notIn({1, 3, 5, 7, 67'000'000'000, 122}));
  filters.push_back(notIn({1, 3, 5, 7, 67'000'000'000, 122}, true));
  filters.push_back(notIn({-4, -3, -2, -1, 0, 1, 2}));
  filters.push_back(notIn({-4, -3, -2, -1, 0, 1, 2}, true));
  filters.push_back(notIn({122, 150, 151, 210, 213, 251}));
  filters.push_back(notIn({122, 150, 151, 210, 213, 251}, true));
  filters.push_back(notIn({0, 1, 3, 9, empty}, false));
  filters.push_back(notIn({0, 1, 3, 9, empty}, true));
  filters.push_back(notIn({empty - 5, empty, empty + 5}, false));
  filters.push_back(notIn({empty - 5, empty, empty + 5}, true));
  filters.push_back(notIn({5, 498, 499, 500}, false));

  for (const auto& left : filters) {
    for (const auto& right : filters) {
      testMergeWithBigint(left.get(), right.get());
    }
  }
}

TEST(FilterTest, mergeWithDouble) {
  std::vector<std::unique_ptr<Filter>> filters;
  addUntypedFilters(filters);

  // Less than.
  filters.push_back(lessThanDouble(1.2));
  filters.push_back(lessThanDouble(1.2, true));
  filters.push_back(lessThanOrEqualDouble(1.2));
  filters.push_back(lessThanOrEqualDouble(1.2, true));

  // Greater than.
  filters.push_back(greaterThanDouble(1.2));
  filters.push_back(greaterThanDouble(1.2, true));
  filters.push_back(greaterThanOrEqualDouble(1.2));
  filters.push_back(greaterThanOrEqualDouble(1.2, true));

  // Between.
  filters.push_back(betweenDouble(-1.2, 3.4));
  filters.push_back(betweenDouble(-1.2, 3.4, true));

  for (const auto& left : filters) {
    for (const auto& right : filters) {
      testMergeWithDouble(left.get(), right.get());
    }
  }
}

TEST(FilterTest, mergeWithFloat) {
  std::vector<std::unique_ptr<Filter>> filters;
  addUntypedFilters(filters);

  // Less than.
  filters.push_back(lessThanFloat(1.2));
  filters.push_back(lessThanFloat(1.2, true));
  filters.push_back(lessThanOrEqualFloat(1.2));
  filters.push_back(lessThanOrEqualFloat(1.2, true));

  // Greater than.
  filters.push_back(greaterThanFloat(1.2));
  filters.push_back(greaterThanFloat(1.2, true));
  filters.push_back(greaterThanOrEqualFloat(1.2));
  filters.push_back(greaterThanOrEqualFloat(1.2, true));

  // Between.
  filters.push_back(betweenFloat(-1.2, 3.4));
  filters.push_back(betweenFloat(-1.2, 3.4, true));

  for (const auto& left : filters) {
    for (const auto& right : filters) {
      testMergeWithFloat(left.get(), right.get());
    }
  }
}

TEST(FilterTest, mergeWithBigintMultiRange) {
  std::vector<std::unique_ptr<Filter>> filters;
  addUntypedFilters(filters);

  filters.push_back(bigintOr(equal(12), between(25, 47)));
  filters.push_back(bigintOr(equal(12), between(25, 47), true));

  filters.push_back(bigintOr(lessThan(12), greaterThan(47)));
  filters.push_back(bigintOr(lessThan(12), greaterThan(47), true));

  filters.push_back(bigintOr(lessThanOrEqual(12), greaterThan(47)));
  filters.push_back(bigintOr(lessThanOrEqual(12), greaterThan(47), true));

  filters.push_back(bigintOr(lessThanOrEqual(12), greaterThanOrEqual(47)));
  filters.push_back(
      bigintOr(lessThanOrEqual(12), greaterThanOrEqual(47), true));

  filters.push_back(bigintOr(lessThan(-3), equal(12), between(25, 47)));
  filters.push_back(bigintOr(lessThan(-3), equal(12), between(25, 47), true));

  // not equal
  filters.push_back(notEqual(20));
  filters.push_back(notEqual(20, true));
  filters.push_back(notEqual(12));
  filters.push_back(notEqual(12, true));
  filters.push_back(notEqual(-210));
  filters.push_back(notEqual(-210, true));

  // not between
  filters.push_back(notBetween(25, 47));
  filters.push_back(notBetween(25, 47, true));
  filters.push_back(notBetween(0, 40));
  filters.push_back(notBetween(30, 40));
  filters.push_back(notBetween(-20, 40));
  filters.push_back(notBetween(12, 40));
  filters.push_back(notBetween(13, 40));
  filters.push_back(notBetween(20, 50));
  filters.push_back(notBetween(-10, -1));
  filters.push_back(notBetween(90, 100));
  filters.push_back(notBetween(std::numeric_limits<int64_t>::min(), -4));
  filters.push_back(notBetween(std::numeric_limits<int64_t>::min(), -4, true));

  // IN-list using bitmask.
  filters.push_back(in({1, 2, 3, 56}));
  filters.push_back(in({1, 2, 3, 56}, true));

  // IN-list using hash table.
  filters.push_back(in({1, 2, 3, 67, 10'134}));
  filters.push_back(in({1, 2, 3, 67, 10'134}, true));
  filters.push_back(
      in({std::numeric_limits<int64_t>::min(),
          0,
          std::numeric_limits<int64_t>::max()},
         true));

  // NOT IN-list using bitmask.
  filters.push_back(notIn({0, 3, 5, 20, 32, 210}));
  filters.push_back(notIn({0, 3, 5, 20, 32, 210}, true));
  filters.push_back(notIn({3, 7, 9, 45, 46, 47, 48}));
  filters.push_back(notIn({3, 7, 9, 45, 46, 47, 48}, true));

  filters.push_back(notIn({12, 18}));
  std::vector<int64_t> rejectionRange;
  rejectionRange.push_back(12);
  for (int i = 25; i <= 47; ++i) {
    rejectionRange.push_back(i);
  }
  filters.push_back(notIn(rejectionRange));

  // NOT IN-list using hash table.
  filters.push_back(notIn({0, 3, 5, 20, 32, 15'210}));
  filters.push_back(notIn({0, 3, 5, 20, 32, 15'210}, true));

  for (const auto& left : filters) {
    for (const auto& right : filters) {
      testMergeWithBigint(left.get(), right.get());
    }
  }
}

TEST(FilterTest, mergeMultiRange) {
  std::vector<std::unique_ptr<Filter>> filters;
  addUntypedFilters(filters);

  filters.push_back(
      orFilter(lessThanOrEqualFloat(-1.2), betweenFloat(1.2, 3.4)));
  filters.push_back(
      orFilter(lessThanOrEqualFloat(-1.2), betweenFloat(1.2, 3.4), true));

  filters.push_back(orFilter(lessThanFloat(1.2), greaterThanFloat(3.4)));
  filters.push_back(orFilter(lessThanFloat(1.2), greaterThanFloat(3.4), true));

  filters.push_back(
      orFilter(lessThanOrEqualFloat(1.2), greaterThanOrEqualFloat(3.4)));
  filters.push_back(
      orFilter(lessThanOrEqualFloat(1.2), greaterThanOrEqualFloat(3.4), true));

  for (const auto& left : filters) {
    for (const auto& right : filters) {
      testMergeWithFloat(left.get(), right.get());
    }
  }
}

TEST(FilterTest, clone) {
  auto filter1 = equal(1, /*nullAllowed=*/false);
  EXPECT_TRUE(filter1->clone(/*nullAllowed*/ true)->testNull());
  auto filter2 = orFilter(
      lessThanOrEqualFloat(-1.2),
      betweenFloat(1.2, 3.4),
      /*nullAllowed=*/false);
  EXPECT_TRUE(filter2->clone(/*nullAllowed*/ true)->testNull());
  auto filter3 = bigintOr(equal(12), between(25, 47), /*nullAllowed=*/false);
  EXPECT_TRUE(filter3->clone(/*nullAllowed*/ true)->testNull());
  auto filter4 = lessThanFloat(1.2, /*nullAllowed=*/false);
  EXPECT_TRUE(filter4->clone(/*nullAllowed*/ true)->testNull());
  auto filter5 = lessThanDouble(1.2, /*nullAllowed=*/false);
  EXPECT_TRUE(filter5->clone(/*nullAllowed*/ true)->testNull());
  auto filter6 = boolEqual(true, /*nullAllowed=*/false);
  EXPECT_TRUE(filter6->clone(/*nullAllowed*/ true)->testNull());
  auto filter7 = betweenFloat(1.2, 1.2, /*nullAllowed=*/false);
  EXPECT_TRUE(filter7->clone(/*nullAllowed*/ true)->testNull());
  auto filter8 = equal("abc", /*nullAllowed=*/false);
  EXPECT_TRUE(filter8->clone(/*nullAllowed*/ true)->testNull());
  std::vector<std::string> values({"Igne", "natura", "renovitur", "integra."});
  auto filter9 = in(values, /*nullAllowed=*/false);
  EXPECT_TRUE(filter9->clone(/*nullAllowed*/ true)->testNull());
  auto filter10 =
      createBigintValues({1, 10, 100, 10'000}, /*nullAllowed=*/false);
  EXPECT_TRUE(filter10->clone(/*nullAllowed*/ true)->testNull());
}

TEST(FilterTest, mergeWithBytesValues) {
  std::vector<std::unique_ptr<Filter>> filters;

  addUntypedFilters(filters);

  filters.push_back(equal("a"));
  filters.push_back(equal("ab"));
  filters.push_back(equal("a", true));

  filters.push_back(in({"e", "f", "g"}));
  filters.push_back(in({"e", "f", "g", "h"}, true));

  filters.push_back(in({"!", "!!jj", ">><<"}));
  filters.push_back(
      in({"!", "!!jj", ">><<", "[]", "12345", "123", "1", "2"}, true));

  filters.push_back(notIn({"a"}));
  filters.push_back(notIn({"a", "b", "c"}));
  filters.push_back(notIn({"e", "g", "!"}, true));
  filters.push_back(notIn({""}));
  filters.push_back(notIn({"a", "b", "c", "d", "e", "f", "g"}));
  filters.push_back(notIn({"!!jj", ">><<", "g", "1234", "12345", "15210"}));

  for (const auto& left : filters) {
    for (const auto& right : filters) {
      testMergeWithBytes(left.get(), right.get());
    }
  }
}

TEST(FilterTest, mergeWithBytesRange) {
  std::vector<std::unique_ptr<Filter>> filters;

  addUntypedFilters(filters);

  filters.push_back(between("abca", "abcc"));
  filters.push_back(between("abca", "abcc", true));

  filters.push_back(between("b", "f"));
  filters.push_back(between("b", "f", true));
  filters.push_back(between("p", "t"));
  filters.push_back(between("p", "t", true));
  filters.push_back(betweenExclusive("a", "z"));
  filters.push_back(betweenExclusive("a", "z", true));

  filters.push_back(between("/", "<"));
  filters.push_back(between("<<", ">>", true));
  filters.push_back(between("ABCDE", "abcde"));
  filters.push_back(between("1", "123456", true));

  filters.push_back(notBetween("b", "f"));
  filters.push_back(notBetween("b", "f", true));
  filters.push_back(notBetween("a", "d"));
  filters.push_back(notBetween("a", "d", true));
  filters.push_back(notBetweenExclusive("b", "f"));
  filters.push_back(notBetweenExclusive("b", "f", true));

  // test unbounded negatedRange
  filters.push_back(
      std::make_unique<facebook::velox::common::NegatedBytesRange>(
          "", true, false, "l", false, false, false));

  filters.push_back(lessThanOrEqual("k"));
  filters.push_back(lessThanOrEqual("k", true));
  filters.push_back(lessThanOrEqual("p"));
  filters.push_back(lessThanOrEqual("p", true));
  filters.push_back(lessThanOrEqual("!!"));
  filters.push_back(lessThanOrEqual("?", true));

  filters.push_back(greaterThanOrEqual("b"));
  filters.push_back(greaterThanOrEqual("b", true));
  filters.push_back(greaterThanOrEqual("e"));
  filters.push_back(greaterThanOrEqual("e", true));

  filters.push_back(lessThan("k"));
  filters.push_back(lessThan("!!", true));
  filters.push_back(lessThan("p"));
  filters.push_back(lessThan("qq", true));
  filters.push_back(lessThan("!>"));
  filters.push_back(lessThan("?", true));

  filters.push_back(greaterThan("b"));
  filters.push_back(greaterThan("b", true));
  filters.push_back(greaterThan("f"));
  filters.push_back(greaterThan("e", true));

  for (const auto& left : filters) {
    for (const auto& right : filters) {
      testMergeWithBytes(left.get(), right.get());
    }
  }
}

TEST(FilterTest, mergeWithBytesMultiRange) {
  std::vector<std::unique_ptr<Filter>> filters;

  addUntypedFilters(filters);

  filters.push_back(between("b", "f"));
  filters.push_back(between("b", "f", true));
  filters.push_back(between("p", "t"));
  filters.push_back(between("p", "t", true));

  filters.push_back(lessThanOrEqual(">"));
  filters.push_back(lessThanOrEqual("k", true));
  filters.push_back(lessThanOrEqual("p"));
  filters.push_back(lessThanOrEqual("<", true));

  filters.push_back(equal("A"));
  filters.push_back(equal("AB"));
  filters.push_back(equal("A", true));

  filters.push_back(in({"e", "f", "!", "h"}));
  filters.push_back(in({"e", "f", "g", "h"}, true));

  // test notIn vs. single ranges and multi-ranges
  filters.push_back(notIn({"a", "c", "g", "t"}));
  filters.push_back(notIn({"k", "", "tt"}));
  filters.push_back(notIn({"bc", "cc", "dc"}));
  filters.push_back(notIn({"+"}, true));
  filters.push_back(notIn({"1", "3", "2"}));
  std::vector<std::string> ends = {"p", "t"};
  filters.push_back(notIn(ends));

  filters.push_back(orFilter(between("!", "f"), greaterThanOrEqual("h")));
  filters.push_back(orFilter(between("b", "f"), lessThanOrEqual("a")));
  filters.push_back(orFilter(between("b", "f"), between("p", "t")));
  filters.push_back(orFilter(lessThanOrEqual("p"), greaterThanOrEqual("y")));
  filters.push_back(orFilter(lessThan("p"), greaterThan("x")));
  filters.push_back(orFilter(betweenExclusive("b", "f"), lessThanOrEqual("a")));

  for (const auto& left : filters) {
    for (const auto& right : filters) {
      testMergeWithBytes(left.get(), right.get());
    }
  }
}

TEST(FilterTest, hugeIntRange) {
  auto filter = equalHugeint(HugeInt::build(1, 1), false);
  auto max = DecimalUtil::kLongDecimalMax;
  auto min = DecimalUtil::kLongDecimalMin;

  EXPECT_TRUE(filter->testInt128(HugeInt::build(1, 1)));
  EXPECT_FALSE(filter->testNull());
  EXPECT_FALSE(filter->testInt128(HugeInt::build(1, 0)));

  EXPECT_TRUE(filter->testInt128Range(
      HugeInt::build(1, 0), HugeInt::build(1, 2), false));
  EXPECT_TRUE(filter->testInt128Range(
      HugeInt::build(1, 0), HugeInt::build(1, 2), true));
  EXPECT_FALSE(filter->testInt128Range(
      HugeInt::build(0, 2), HugeInt::build(1, 0), false));
  EXPECT_FALSE(filter->testInt128Range(
      HugeInt::build(1, 2), HugeInt::build(1, 2), true));

  filter = equalHugeint(max, false);
  EXPECT_TRUE(filter->testInt128(max));
  EXPECT_FALSE(filter->testInt128(min));
  EXPECT_TRUE(filter->testInt128Range(HugeInt::build(1, 2), max, false));
  EXPECT_FALSE(filter->testInt128Range(min, HugeInt::build(1, 2), false));

  filter = equalHugeint(min, false);
  EXPECT_TRUE(filter->testInt128(min));
  EXPECT_FALSE(filter->testInt128(max));
  EXPECT_TRUE(filter->testInt128Range(min, HugeInt::build(1, 2), false));
  EXPECT_FALSE(filter->testInt128Range(HugeInt::build(1, 2), max, false));

  filter = betweenHugeint(HugeInt::build(1, 1), HugeInt::build(2, 2), false);
  EXPECT_TRUE(filter->testInt128(HugeInt::build(1, 3)));
  EXPECT_TRUE(filter->testInt128(HugeInt::build(2, 1)));
  EXPECT_FALSE(filter->testNull());
  EXPECT_FALSE(filter->testInt128(HugeInt::build(1, 0)));
  EXPECT_FALSE(filter->testInt128(HugeInt::build(2, 3)));

  EXPECT_TRUE(filter->testInt128Range(
      HugeInt::build(1, 3), HugeInt::build(2, 0), false));
  EXPECT_TRUE(filter->testInt128Range(
      HugeInt::build(2, 0), HugeInt::build(2, 2), true));
  EXPECT_FALSE(filter->testInt128Range(
      HugeInt::build(0, 0), HugeInt::build(1, 0), false));
  EXPECT_FALSE(filter->testInt128Range(
      HugeInt::build(2, 3), HugeInt::build(3, 5), true));

  filter = betweenHugeint(HugeInt::build(1, 1), max, false);
  EXPECT_TRUE(filter->testInt128(max));
  EXPECT_FALSE(filter->testInt128(min));
  EXPECT_TRUE(filter->testInt128Range(HugeInt::build(2, 2), max, true));
  EXPECT_FALSE(filter->testInt128Range(min, HugeInt::build(1, 0), false));

  filter = betweenHugeint(min, HugeInt::build(1, 1), false);
  EXPECT_TRUE(filter->testInt128(min));
  EXPECT_FALSE(filter->testInt128(max));
  EXPECT_TRUE(filter->testInt128Range(min, HugeInt::build(0, 1), true));
  EXPECT_FALSE(filter->testInt128Range(HugeInt::build(1, 2), max, false));

  filter = greaterThanHugeint(HugeInt::build(1, 1), true);
  EXPECT_TRUE(filter->testNull());
  EXPECT_FALSE(filter->testInt128(HugeInt::build(1, 0)));
  EXPECT_TRUE(filter->testInt128(HugeInt::build(1, 2)));

  EXPECT_TRUE(filter->testInt128Range(
      HugeInt::build(1, 0), HugeInt::build(1, 2), false));
  EXPECT_TRUE(filter->testInt128Range(
      HugeInt::build(0, 0), HugeInt::build(1, 1), true));
  EXPECT_TRUE(filter->testInt128Range(
      HugeInt::build(2, 1), HugeInt::build(2, 2), true));
  EXPECT_FALSE(filter->testInt128Range(
      HugeInt::build(0, 0), HugeInt::build(0, 1), false));

  filter = greaterThanHugeint(min, true);
  EXPECT_TRUE(filter->testInt128(max));
  EXPECT_FALSE(filter->testInt128(min));
  EXPECT_TRUE(filter->testInt128Range(min, max, false));

  filter = greaterThanHugeint(max, true);
  EXPECT_FALSE(filter->testInt128(max));
  EXPECT_FALSE(filter->testInt128Range(min, max, false));
}

TEST(FilterTest, dateRange) {
  auto filter =
      between(DATE()->toDays("1970-01-01"), DATE()->toDays("1980-01-01"));
  EXPECT_TRUE(applyFilter(*filter, DATE()->toDays("1970-06-01")));
  EXPECT_FALSE(applyFilter(*filter, DATE()->toDays("1980-06-01")));
}

TEST(FilterTest, timestampRange) {
  // x = timestamp '1970-01-01 00:00:10.123'
  auto filter = equal(Timestamp(10, 123000000), false);
  EXPECT_FALSE(filter->testNull());

  EXPECT_TRUE(filter->testTimestamp(Timestamp(10, 123000000)));
  EXPECT_FALSE(filter->testTimestamp(Timestamp()));
  EXPECT_FALSE(filter->testTimestamp(Timestamp(-10, 123000000)));

  EXPECT_FALSE(filter->testTimestampRange(
      Timestamp(-777, 123450000), Timestamp(9, 123000000), false));
  EXPECT_FALSE(filter->testTimestampRange(
      Timestamp(10, 125000000), Timestamp(999, 12), false));
  EXPECT_TRUE(filter->testTimestampRange(
      Timestamp(-123, 123450000), Timestamp(123, 123450000), false));
  EXPECT_TRUE(filter->testTimestampRange(
      Timestamp(10, 122000000), Timestamp(10, 124000000), false));

  // x between timestamp '1970-01-01 00:00:10.123' and timestamp '1970-01-01
  // 00:02:00.456'
  filter = between(Timestamp(10, 123000000), Timestamp(120, 456000000), false);
  EXPECT_FALSE(filter->testNull());

  EXPECT_TRUE(filter->testTimestamp(Timestamp(10, 123000001)));
  EXPECT_TRUE(filter->testTimestamp(Timestamp(111, 999999999)));
  EXPECT_FALSE(filter->testTimestamp(Timestamp()));
  EXPECT_FALSE(filter->testTimestamp(Timestamp(123, 0)));

  EXPECT_FALSE(filter->testTimestampRange(
      Timestamp(-123, 123450000), Timestamp(9, 123450000), false));
  EXPECT_FALSE(filter->testTimestampRange(
      Timestamp(-123, 123450000), Timestamp(9, 123450000), true));
  EXPECT_TRUE(filter->testTimestampRange(
      Timestamp(-123, 123450000), Timestamp(20, 123450000), false));
  EXPECT_TRUE(filter->testTimestampRange(
      Timestamp(111, 999999999), Timestamp(123, 777777777), false));

  //  x > timestamp '1970-01-01 00:00:10.123' or x = null
  Timestamp ts(10, 123000000);
  filter = greaterThan(ts, true);
  EXPECT_TRUE(filter->testNull());

  EXPECT_FALSE(filter->testTimestamp(Timestamp()));
  EXPECT_TRUE(filter->testTimestamp(Timestamp(10, 124000000)));

  EXPECT_FALSE(filter->testTimestampRange(
      Timestamp(-123, 123450000), Timestamp(9, 123000000), false));
  EXPECT_TRUE(filter->testTimestampRange(
      Timestamp(-123, 123450000), Timestamp(9, 123000000), true));
  EXPECT_TRUE(filter->testTimestampRange(
      Timestamp(5, 123000000), Timestamp(30, 123000000), false));
  EXPECT_TRUE(filter->testTimestampRange(
      Timestamp(5, 123000000), Timestamp(30, 123000000), true));
}
