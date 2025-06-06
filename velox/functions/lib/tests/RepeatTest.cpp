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

#include "velox/functions/lib/Repeat.h"
#include "velox/common/base/tests/GTestUtils.h"
#include "velox/common/testutil/OptionalEmpty.h"
#include "velox/functions/prestosql/tests/utils/FunctionBaseTest.h"

using namespace facebook::velox::test;

namespace facebook::velox::functions {
namespace {

class RepeatTest : public functions::test::FunctionBaseTest {
 protected:
  static void SetUpTestCase() {
    FunctionBaseTest::SetUpTestCase();
    exec::registerStatefulVectorFunction(
        "repeat",
        functions::repeatSignatures(),
        functions::makeRepeat,
        functions::repeatMetadata());
    exec::registerStatefulVectorFunction(
        "repeat_allow_negative_count",
        functions::repeatSignatures(),
        functions::makeRepeatAllowNegativeCount,
        functions::repeatMetadata());
  }

  void testExpression(
      const std::string& expression,
      const std::vector<VectorPtr>& input,
      const VectorPtr& expected) {
    auto result = evaluate(expression, makeRowVector(input));
    assertEqualVectors(expected, result);
  }

  void testExpressionWithError(
      const std::string& expression,
      const std::vector<VectorPtr>& input,
      const std::string& expectedError) {
    VELOX_ASSERT_THROW(
        evaluate(expression, makeRowVector(input)), expectedError);
  }
};

TEST_F(RepeatTest, repeat) {
  const auto elementVector = makeNullableFlatVector<float>(
      {0.0, -2.0, 3.333333, 4.0004, std::nullopt, 5.12345});
  const auto countVector =
      makeNullableFlatVector<int32_t>({1, 2, 3, 0, 4, std::nullopt});
  VectorPtr expected;

  expected = makeNullableArrayVector<float>({
      {{0.0}},
      {{-2.0, -2.0}},
      {{3.333333, 3.333333, 3.333333}},
      common::testutil::optionalEmpty,
      {{std::nullopt, std::nullopt, std::nullopt, std::nullopt}},
      std::nullopt,
  });
  testExpression("repeat(C0, C1)", {elementVector, countVector}, expected);
  testExpression("try(repeat(C0, C1))", {elementVector, countVector}, expected);

  // Test using a null constant as the count argument.
  expected = BaseVector::createNullConstant(ARRAY(REAL()), 6, pool());
  testExpression("repeat(C0, null::INTEGER)", {elementVector}, expected);
  testExpression("try(repeat(C0, null::INTEGER))", {elementVector}, expected);

  // Test using a non-null constant as the count argument.
  expected = makeNullableArrayVector<float>({
      {0.0, 0.0, 0.0},
      {-2.0, -2.0, -2.0},
      {3.333333, 3.333333, 3.333333},
      {4.0004, 4.0004, 4.0004},
      {std::nullopt, std::nullopt, std::nullopt},
      {5.12345, 5.12345, 5.12345},
  });
  testExpression("repeat(C0, '3'::INTEGER)", {elementVector}, expected);
  testExpression("try(repeat(C0, '3'::INTEGER))", {elementVector}, expected);

  expected = makeArrayVector<float>({{}, {}, {}, {}, {}, {}});
  testExpression("repeat(C0, '0'::INTEGER)", {elementVector}, expected);
  testExpression("try(repeat(C0, '0'::INTEGER))", {elementVector}, expected);
}

TEST_F(RepeatTest, repeatWithEntriesWithMaxElementsSize) {
  const auto elementVector =
      makeNullableFlatVector<float>({0.0, 2.0, std::nullopt, 3.0});
  execCtx_.queryCtx()->testingOverrideConfigUnsafe({
      {core::QueryConfig::kMaxElementsSizeInRepeatAndSequence, "15000"},
  });

  const int32_t within_limit = 13'000;
  auto countVectorWithinLimit = makeNullableFlatVector<int32_t>(
      {within_limit, within_limit, within_limit, std::nullopt});

  auto withinLimitData = makeRowVector({elementVector, countVectorWithinLimit});
  auto typedExprWithinLimit =
      makeTypedExpr("repeat(C0, C1)", asRowType(withinLimitData->type()));

  exec::ExprSet exprSetWithinLimit({typedExprWithinLimit}, &execCtx_);
  exec::EvalCtx evalCtxWithinLimit(
      &execCtx_, &exprSetWithinLimit, withinLimitData.get());

  auto result = evaluate(exprSetWithinLimit, withinLimitData);

  VectorPtr expected = makeNullableArrayVector<float>({
      std::vector<std::optional<float>>(within_limit, 0.0),
      std::vector<std::optional<float>>(within_limit, 2.0),
      std::vector<std::optional<float>>(within_limit, std::nullopt),
      std::nullopt,
  });

  assertEqualVectors(expected, result);

  const int32_t over_limit = 15'200;
  auto countVectorOverLimit = makeNullableFlatVector<int32_t>(
      {over_limit, over_limit, over_limit, std::nullopt});

  auto overLimitData = makeRowVector({elementVector, countVectorOverLimit});
  auto typedExprWithOverLimit =
      makeTypedExpr("repeat(C0, C1)", asRowType(overLimitData->type()));

  exec::ExprSet exprSetOverLimit({typedExprWithOverLimit}, &execCtx_);
  exec::EvalCtx evalCtx(&execCtx_, &exprSetOverLimit, overLimitData.get());

  VELOX_ASSERT_THROW(
      evaluate(exprSetOverLimit, overLimitData),
      "Count argument of repeat function must be less than or equal to 15000");
}

TEST_F(RepeatTest, repeatWithInvalidCount) {
  const auto elementVector =
      makeNullableFlatVector<float>({0.0, 2.0, 3.333333});

  VectorPtr countVector;
  VectorPtr expected;

  expected = makeNullableArrayVector<float>({
      {{0.0}},
      std::nullopt,
      {{3.333333, 3.333333, 3.333333}},
  });
  countVector = makeNullableFlatVector<int32_t>({1, -2, 3});
  testExpression("try(repeat(C0, C1))", {elementVector, countVector}, expected);
  testExpressionWithError(
      "repeat(C0, C1)",
      {elementVector, countVector},
      "(-2 vs. 0) Count argument of repeat function must be greater than or equal to 0");

  countVector = makeNullableFlatVector<int32_t>({1, 123456, 3});
  testExpression("try(repeat(C0, C1))", {elementVector, countVector}, expected);
  testExpressionWithError(
      "repeat(C0, C1)",
      {elementVector, countVector},
      "(123456 vs. 10000) Count argument of repeat function must be less than or equal to 10000");

  // Test using a constant as the count argument.
  expected = BaseVector::createNullConstant(ARRAY(REAL()), 3, pool());
  testExpression("try(repeat(C0, '-5'::INTEGER))", {elementVector}, expected);
  testExpressionWithError(
      "repeat(C0, '-5'::INTEGER)",
      {elementVector},
      "(-5 vs. 0) Count argument of repeat function must be greater than or equal to 0");

  testExpression(
      "try(repeat(C0, '10001'::INTEGER))", {elementVector}, expected);
  testExpressionWithError(
      "repeat(C0, '10001'::INTEGER)",
      {elementVector},
      "(10001 vs. 10000) Count argument of repeat function must be less than or equal to 10000");
}

TEST_F(RepeatTest, repeatAllowNegativeCount) {
  const auto elementVector = makeNullableFlatVector<float>(
      {0.0, -2.0, 3.333333, 4.0004, std::nullopt, 5.12345});
  auto expected = makeArrayVector<float>({{}, {}, {}, {}, {}, {}});

  // Test negative count.
  auto countVector =
      makeNullableFlatVector<int32_t>({-1, -2, -3, -5, -10, -100});
  testExpression(
      "repeat_allow_negative_count(C0, C1)",
      {elementVector, countVector},
      expected);

  // Test using a constant as the count argument.
  testExpression(
      "repeat_allow_negative_count(C0, '-5'::INTEGER)",
      {elementVector},
      expected);

  // Test mixed case.
  expected = makeArrayVector<float>(
      {{0.0}, {-2.0, -2.0}, {}, {}, {}, {5.12345, 5.12345, 5.12345}});
  countVector = makeNullableFlatVector<int32_t>({1, 2, -1, 0, -10, 3});
  testExpression(
      "repeat_allow_negative_count(C0, C1)",
      {elementVector, countVector},
      expected);
}
} // namespace
} // namespace facebook::velox::functions
