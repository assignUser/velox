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
#include "velox/common/base/tests/GTestUtils.h"
#include "velox/functions/prestosql/tests/utils/FunctionBaseTest.h"

using namespace facebook::velox;
using namespace facebook::velox::test;
using namespace facebook::velox::exec;
using namespace facebook::velox::functions::test;

namespace {

class ArrayFlattenTest : public FunctionBaseTest {
 protected:
  void testExpression(
      const std::string& expression,
      const std::vector<VectorPtr>& input,
      const VectorPtr& expected) {
    auto result = evaluate(expression, makeRowVector(input));
    assertEqualVectors(expected, result);
  }
};

/// Flatten integer arrays.
TEST_F(ArrayFlattenTest, intArrays) {
  const auto baseVector = makeArrayVectorFromJson<int64_t>(
      {"[1, 1]", "[2,2]", "[3, 3]", "[4,4]", "[5, 5]", "[6, 6]"});

  // Create arrays of array vector using above base vector.
  // [[1, 1], [2, 2], [3, 3]]
  // [[4, 4]]
  // [[5, 5], [6, 6]]
  const auto arrayOfArrays = makeArrayVector({0, 3, 4}, baseVector);

  // [1, 1, 2, 2, 3, 3]
  // [4, 4]
  // [5, 5, 6, 6]
  const auto expected =
      makeArrayVector<int64_t>({{1, 1, 2, 2, 3, 3}, {4, 4}, {5, 5, 6, 6}});

  testExpression("flatten(c0)", {arrayOfArrays}, expected);
}

/// Flatten arrays with null.
TEST_F(ArrayFlattenTest, nullArrayWithConsecutiveBaseElements) {
  const auto baseVector = makeNullableArrayVector<int64_t>(
      {{{1, 1}},
       std::nullopt,
       {{3, 3}},
       {{5, std::nullopt}},
       {{std::nullopt, 6}}});

  // Create arrays of array vector using above base vector.
  // [[1, 1], null, [3, 3]]
  // null
  // [[5, null], [null, 6]]
  const auto arrayOfArrays = makeArrayVector({0, 3, 3}, baseVector, {1});

  // [[1, 1, 3, 3]]
  // null
  // [[5, null, null, 6]]
  const auto expected = makeNullableArrayVector<int64_t>(
      {{{1, 1, 3, 3}}, std::nullopt, {{5, std::nullopt, std::nullopt, 6}}});

  testExpression("flatten(c0)", {arrayOfArrays}, expected);
}

TEST_F(ArrayFlattenTest, nullArrayWithoutConsecutiveBaseElements) {
  const auto baseVector = makeArrayVectorFromJson<int64_t>(
      {"[1, 1]", "null", "[3, 3]", "[]", "[5, null]", "[null, 6]"});

  // Create arrays of array vector using above base vector.
  const auto reversedIndices = makeIndicesInReverse(6);
  const auto mappedVector = wrapInDictionary(reversedIndices, baseVector);

  // Create arrays of array vector using above mapped vector.
  // [[null, 6], [5, null]]
  // null
  // [[3, 3], null, [1, 1]]
  const auto arrayOfArrays = makeArrayVector({0, 3, 3}, mappedVector, {1});

  // [[null, 6, 5, null]]
  // null
  // [[3, 3, 1, 1]]
  const auto expected = makeNullableArrayVector<int64_t>(
      {{{std::nullopt, 6, 5, std::nullopt}}, std::nullopt, {{3, 3, 1, 1}}});

  testExpression("flatten(c0)", {arrayOfArrays}, expected);
}

TEST_F(ArrayFlattenTest, constantArrayWithConsecutiveBaseElements) {
  const auto baseVector = makeArrayVectorFromJson<int64_t>(
      {"[1, 1]", "null", "[3, 3]", "[]", "[5, null]", "[null, 6]"});
  // Create arrays of array vector using above base vector.
  // [[1, 1], null, [3, 3]]
  // null
  // [[], [5, null], [null, 6]]
  const auto arrayOfArrays = makeArrayVector({0, 3, 3}, baseVector, {1});

  // Create three element constant array by referencing the first element in
  // 'arrayOfArrays'.
  const auto constArrayOfArrays =
      BaseVector::wrapInConstant(3, 0, arrayOfArrays);
  // [[1, 1, 3, 3]]
  // [[1, 1, 3, 3]]
  // [[1, 1, 3, 3]]
  const auto expected = makeArrayVector<int64_t>(
      {{{1, 1, 3, 3}}, {{1, 1, 3, 3}}, {{1, 1, 3, 3}}});

  // Enforce the constant encoding flatten path to trigger.
  testExpression("flatten(c0)", {constArrayOfArrays}, expected);
}

TEST_F(ArrayFlattenTest, constantArrayWithoutConsecutiveBaseElements) {
  const auto baseVector = makeArrayVectorFromJson<int64_t>(
      {"[1, 1]", "null", "[3, 3]", "[]", "[5, null]", "[null, 6]"});
  // Create arrays of array vector using above base vector.
  const auto reversedIndices = makeIndicesInReverse(6);
  const auto mappedVector = wrapInDictionary(reversedIndices, baseVector);
  // Create arrays of array vector using above base vector.
  // [[null, 6], [5, null], []]
  // null
  // [[3, 3], null, [1, 1]]
  const auto arrayOfArrays = makeArrayVector({0, 3, 3}, mappedVector, {1});

  // Create three element constant array by referencing the first element in
  // 'arrayOfArrays'.
  const auto constArrayOfArrays =
      BaseVector::wrapInConstant(3, 0, arrayOfArrays);
  // [[null, 6, 5, null]
  // [[null, 6, 5, null]
  // [[null, 6, 5, null]
  const auto expected = makeNullableArrayVector<int64_t>(
      std::vector<std::vector<std::optional<int64_t>>>{
          {std::nullopt, 6, 5, std::nullopt},
          {std::nullopt, 6, 5, std::nullopt},
          {std::nullopt, 6, 5, std::nullopt}});
  testExpression("flatten(c0)", {constArrayOfArrays}, expected);
}
} // namespace
