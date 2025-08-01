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
#include <folly/base64.h>
#include "velox/core/Expressions.h"
#include "velox/exec/tests/utils/PlanBuilder.h"
#include "velox/functions/lib/QuantileDigest.h"
#include "velox/functions/lib/TDigest.h"
#include "velox/functions/lib/aggregates/tests/utils/AggregationTestBase.h"
#include "velox/functions/prestosql/aggregates/sfm/SfmSketch.h"
#include "velox/functions/prestosql/types/QDigestRegistration.h"
#include "velox/functions/prestosql/types/QDigestType.h"
#include "velox/functions/prestosql/types/SfmSketchRegistration.h"
#include "velox/functions/prestosql/types/SfmSketchType.h"
#include "velox/functions/prestosql/types/TDigestRegistration.h"
#include "velox/functions/prestosql/types/TDigestType.h"

using namespace facebook::velox::exec;
using namespace facebook::velox::exec::test;
using namespace facebook::velox::functions::aggregate::test;
using namespace facebook::velox::functions::qdigest;
using SfmSketchIn = facebook::velox::functions::aggregate::SfmSketch;

namespace facebook::velox::aggregate::test {
namespace {

class MergeAggregateTest : public AggregationTestBase {
 protected:
  // Helper Methods for TDigest and QDigest
  static const std::vector<double> kTDigestQuantiles;

  facebook::velox::functions::TDigest<> createTDigest(const char* inputData) {
    facebook::velox::functions::TDigest<> tdigest;
    std::vector<int16_t> positions;
    tdigest.mergeDeserialized(positions, inputData);
    tdigest.compress(positions);
    return tdigest;
  }

  // Function to check TDigest equality similar to Java's
  // TDIGEST_EQUALITY
  bool tdigestEquals(
      const std::string& actualBase64,
      const std::string& expectedBase64) {
    if (actualBase64.empty() && expectedBase64.empty()) {
      return true;
    }
    if (actualBase64.empty() || expectedBase64.empty()) {
      return false;
    }

    // Decode base64 strings to binary
    auto actualBinary = folly::base64Decode(actualBase64);
    auto expectedBinary = folly::base64Decode(expectedBase64);

    // Create TDigest objects
    facebook::velox::functions::TDigest<> actual =
        createTDigest(actualBinary.data());
    facebook::velox::functions::TDigest<> expected =
        createTDigest(expectedBinary.data());

    // Compare quantiles
    for (double quantile : kTDigestQuantiles) {
      double actualQuantile = actual.estimateQuantile(quantile);
      double expectedQuantile = expected.estimateQuantile(quantile);
      double tolerance = std::max(1.0, std::abs(actualQuantile * 0.01));
      if (std::abs(actualQuantile - expectedQuantile) > tolerance) {
        return false;
      }
    }

    // Compare sum with tolerance
    if (std::abs(actual.sum() - expected.sum()) > 0.0001) {
      return false;
    }

    // Compare other properties
    return actual.totalWeight() == expected.totalWeight() &&
        actual.min() == expected.min() && actual.max() == expected.max() &&
        actual.compression() == expected.compression();
  }

  template <typename T>
  facebook::velox::functions::qdigest::QuantileDigest<T, std::allocator<T>>
  createQDigest(const char* inputData) {
    if (inputData == nullptr || *inputData == '\0') {
      // Use uninitialized max error, if empty input data
      return facebook::velox::functions::qdigest::
          QuantileDigest<T, std::allocator<T>>(
              std::allocator<T>(),
              facebook::velox::functions::qdigest::kUninitializedMaxError);
    }
    return facebook::velox::functions::qdigest::
        QuantileDigest<T, std::allocator<T>>(std::allocator<T>(), inputData);
  }

  // Function to check QDigest equality
  template <typename T>
  bool qdigestEquals(
      const std::string& actualBase64,
      const std::string& expectedBase64) {
    if (actualBase64.empty() && expectedBase64.empty()) {
      return true;
    }
    if (actualBase64.empty() || expectedBase64.empty()) {
      return false;
    }
    auto actualBinary = folly::base64Decode(actualBase64);
    auto expectedBinary = folly::base64Decode(expectedBase64);
    // Create QDigest objects
    auto actual = createQDigest<T>(actualBinary.data());
    auto expected = createQDigest<T>(expectedBinary.data());
    // Use same quantiles as in QuantileDigestTestBase
    const double quantiles[] = {
        0.0001, 0.0200, 0.0300, 0.04000, 0.0500, 0.1000, 0.2000,
        0.3000, 0.4000, 0.5000, 0.6000,  0.7000, 0.8000, 0.9000,
        0.9500, 0.9600, 0.9700, 0.9800,  0.9999,
    };
    // Compare value at quantiles
    const double epsilon = 0.00001;
    for (auto q : quantiles) {
      auto actualValue = actual.estimateQuantile(q);
      auto expectedValue = expected.estimateQuantile(q);
      // Use double to avoid overflow
      double absExpected = std::abs(static_cast<double>(expectedValue));
      if (absExpected > 1e-6) {
        double relativeError = std::abs(
            static_cast<double>(actualValue - expectedValue) / expectedValue);
        if (relativeError > epsilon) {
          return false;
        }
      } else {
        if (std::abs(static_cast<double>(actualValue - expectedValue)) >
            epsilon) {
          return false;
        }
      }
    }
    return true;
  }

  // Helper Methods for both types
  template <typename T>
  core::PlanNodePtr createMergePlan(
      const RowVectorPtr& input,
      const TypePtr& digestType) {
    auto fieldAccess =
        std::make_shared<core::FieldAccessTypedExpr>(VARCHAR(), "c0");
    auto fromBase64 = std::make_shared<core::CallTypedExpr>(
        VARBINARY(),
        std::vector<core::TypedExprPtr>{fieldAccess},
        "from_base64");
    auto castToDigest =
        std::make_shared<core::CastTypedExpr>(digestType, fromBase64, true);
    auto aggResultAccess =
        std::make_shared<core::FieldAccessTypedExpr>(digestType, "a0");
    auto castToVarbinary = std::make_shared<core::CastTypedExpr>(
        VARBINARY(), aggResultAccess, true);
    auto toBase64 = std::make_shared<core::CallTypedExpr>(
        VARCHAR(),
        std::vector<core::TypedExprPtr>{castToVarbinary},
        "to_base64");

    return PlanBuilder()
        .values({input})
        .projectExpressions({castToDigest})
        .singleAggregation({}, {"merge(p0)"})
        .projectExpressions({toBase64})
        .planNode();
  }
};

const std::vector<double> MergeAggregateTest::kTDigestQuantiles =
    {0.01, 0.05, 0.1, 0.25, 0.50, 0.75, 0.9, 0.95, 0.99};

// TDigest merge tests
TEST_F(MergeAggregateTest, mergeTDigestMatchJava) {
  registerTDigestType();
  // Base64-encoded TDigests from Presto Java
  // TDigest: values 1.0 - 50.0
  const std::string kTDigest1 =
      "AQAAAAAAAADwPwAAAAAAAElAAAAAAADsk0AAAAAAAABZQAAAAAAAAElAJgAAAAAAAAAAAPA/AAAAAAAA8D8AAAAAAADwPwAAAAAAAPA/AAAAAAAA8D8AAAAAAADwPwAAAAAAAPA/AAAAAAAA8D8AAAAAAADwPwAAAAAAAPA/AAAAAAAA8D8AAAAAAADwPwAAAAAAAPA/AAAAAAAAAEAAAAAAAAAAQAAAAAAAAABAAAAAAAAAAEAAAAAAAAAAQAAAAAAAAABAAAAAAAAAAEAAAAAAAAAAQAAAAAAAAABAAAAAAAAAAEAAAAAAAAAAQAAAAAAAAABAAAAAAAAA8D8AAAAAAADwPwAAAAAAAPA/AAAAAAAA8D8AAAAAAADwPwAAAAAAAPA/AAAAAAAA8D8AAAAAAADwPwAAAAAAAPA/AAAAAAAA8D8AAAAAAADwPwAAAAAAAPA/AAAAAAAA8D8AAAAAAADwPwAAAAAAAABAAAAAAAAACEAAAAAAAAAQQAAAAAAAABRAAAAAAAAAGEAAAAAAAAAcQAAAAAAAACBAAAAAAAAAIkAAAAAAAAAkQAAAAAAAACZAAAAAAAAAKEAAAAAAAAAqQAAAAAAAAC1AAAAAAACAMEAAAAAAAIAyQAAAAAAAgDRAAAAAAACANkAAAAAAAIA4QAAAAAAAgDpAAAAAAACAPEAAAAAAAIA+QAAAAAAAQEBAAAAAAABAQUAAAAAAAEBCQAAAAAAAAENAAAAAAACAQ0AAAAAAAABEQAAAAAAAgERAAAAAAAAARUAAAAAAAIBFQAAAAAAAAEZAAAAAAACARkAAAAAAAABHQAAAAAAAgEdAAAAAAAAASEAAAAAAAIBIQAAAAAAAAElA";
  // TDigest: values 51.0 - 100.0
  const std::string kTDigest2 =
      "AQAAAAAAAIBJQAAAAAAAAFlAAAAAAAB+rUAAAAAAAABZQAAAAAAAAElAJgAAAAAAAAAAAPA/AAAAAAAA8D8AAAAAAADwPwAAAAAAAPA/AAAAAAAA8D8AAAAAAADwPwAAAAAAAPA/AAAAAAAA8D8AAAAAAADwPwAAAAAAAPA/AAAAAAAA8D8AAAAAAADwPwAAAAAAAPA/AAAAAAAAAEAAAAAAAAAAQAAAAAAAAABAAAAAAAAAAEAAAAAAAAAAQAAAAAAAAABAAAAAAAAAAEAAAAAAAAAAQAAAAAAAAABAAAAAAAAAAEAAAAAAAAAAQAAAAAAAAABAAAAAAAAA8D8AAAAAAADwPwAAAAAAAPA/AAAAAAAA8D8AAAAAAADwPwAAAAAAAPA/AAAAAAAA8D8AAAAAAADwPwAAAAAAAPA/AAAAAAAA8D8AAAAAAADwPwAAAAAAAPA/AAAAAAAA8D8AAAAAAIBJQAAAAAAAAEpAAAAAAACASkAAAAAAAABLQAAAAAAAgEtAAAAAAAAATEAAAAAAAIBMQAAAAAAAAE1AAAAAAACATUAAAAAAAABOQAAAAAAAgE5AAAAAAAAAT0AAAAAAAIBPQAAAAAAAIFBAAAAAAACgUEAAAAAAACBRQAAAAAAAoFFAAAAAAAAgUkAAAAAAAKBSQAAAAAAAIFNAAAAAAACgU0AAAAAAACBUQAAAAAAAoFRAAAAAAAAgVUAAAAAAAKBVQAAAAAAAAFZAAAAAAABAVkAAAAAAAIBWQAAAAAAAwFZAAAAAAAAAV0AAAAAAAEBXQAAAAAAAgFdAAAAAAADAV0AAAAAAAABYQAAAAAAAQFhAAAAAAACAWEAAAAAAAMBYQAAAAAAAAFlA";

  // Create TDigest vectors from base64 strings
  auto tdigestData = makeFlatVector<std::string>({kTDigest1, kTDigest2});

  // Create plan and execute
  auto op =
      createMergePlan<double>(makeRowVector({tdigestData}), TDIGEST(DOUBLE()));
  auto result = readSingleValue(op);

  // Expected merged TDigest base64 string from Java
  const std::string expectedMergedTDigest =
      "AQAAAAAAAADwPwAAAAAAAFlAAAAAAAC6s0AAAAAAAABZQAAAAAAAAFlAKgAAAAAAAAAAAPA/AAAAAAAA8D8AAAAAAADwPwAAAAAAAPA/AAAAAAAA8D8AAAAAAADwPwAAAAAAAPA/AAAAAAAA8D8AAAAAAADwPwAAAAAAAPA/AAAAAAAAAEAAAAAAAAAAQAAAAAAAAABAAAAAAAAACEAAAAAAAAAIQAAAAAAAABBAAAAAAAAAEEAAAAAAAAAUQAAAAAAAABRAAAAAAAAAFEAAAAAAAAAUQAAAAAAAABRAAAAAAAAAFEAAAAAAAAAUQAAAAAAAABRAAAAAAAAAEEAAAAAAAAAQQAAAAAAAAAhAAAAAAAAACEAAAAAAAAAAQAAAAAAAAABAAAAAAAAAAEAAAAAAAADwPwAAAAAAAPA/AAAAAAAA8D8AAAAAAADwPwAAAAAAAPA/AAAAAAAA8D8AAAAAAADwPwAAAAAAAPA/AAAAAAAA8D8AAAAAAADwPwAAAAAAAPA/AAAAAAAAAEAAAAAAAAAIQAAAAAAAABBAAAAAAAAAFEAAAAAAAAAYQAAAAAAAABxAAAAAAAAAIEAAAAAAAAAiQAAAAAAAACRAAAAAAAAAJ0AAAAAAAAArQAAAAAAAAC9AAAAAAAAAMkAAAAAAAAA1QAAAAAAAgDhAAAAAAACAPEAAAAAAAIBAQAAAAAAAAENAAAAAAACARUAAAAAAAABIQAAAAAAAgEpAAAAAAAAATUAAAAAAAIBPQAAAAAAAAFFAAAAAAAAgUkAAAAAAACBTQAAAAAAAAFRAAAAAAADAVEAAAAAAAGBVQAAAAAAA4FVAAAAAAABgVkAAAAAAAMBWQAAAAAAAAFdAAAAAAABAV0AAAAAAAIBXQAAAAAAAwFdAAAAAAAAAWEAAAAAAAEBYQAAAAAAAgFhAAAAAAADAWEAAAAAAAABZQA==";

  ASSERT_TRUE(
      tdigestEquals(result.value<TypeKind::VARCHAR>(), expectedMergedTDigest));
}

TEST_F(MergeAggregateTest, mergeTDigestOneNullMatchJava) {
  registerTDigestType();
  // Base64-encoded TDigests from Presto Java
  // TDigest: values 1.0, 2.0, 3.0
  const std::string kTDigest1 =
      "AQAAAAAAAADwPwAAAAAAAAhAAAAAAAAAGEAAAAAAAABZQAAAAAAAAAhAAwAAAAAAAAAAAPA/AAAAAAAA8D8AAAAAAADwPwAAAAAAAPA/AAAAAAAAAEAAAAAAAAAIQA==";
  // Null TDigest
  const std::string kTDigest2;

  // Create TDigest vectors from base64 strings
  auto tdigestData = makeFlatVector<std::string>({kTDigest1, kTDigest2});

  // Create plan and execute
  auto op =
      createMergePlan<double>(makeRowVector({tdigestData}), TDIGEST(DOUBLE()));
  auto result = readSingleValue(op);

  // Expected merged TDigest base64 string from Java
  const std::string expectedMergedTDigest =
      "AQAAAAAAAADwPwAAAAAAAAhAAAAAAAAAGEAAAAAAAABZQAAAAAAAAAhAAwAAAAAAAAAAAPA/AAAAAAAA8D8AAAAAAADwPwAAAAAAAPA/AAAAAAAAAEAAAAAAAAAIQA==";

  ASSERT_TRUE(
      tdigestEquals(result.value<TypeKind::VARCHAR>(), expectedMergedTDigest));
}

// QDigest merge tests
TEST_F(MergeAggregateTest, mergeQDigestDoubleMatchJava) {
  registerQDigestType();
  // Base64-encoded QDigests from Presto Java
  // QDigest: values 1 to 50
  const std::string kQDigest1 =
      "AK5H4XoUru8/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAPA/AAAAAAAASUBjAAAAAAAAAAAAAPA/AAAAAAAA8L8AAAAAAAAA8D8AAAAAAAAAwAAAAAAAAADwPwAAAAAAAAjAzwAAAAAAAAAAAAAAAAAAAMAAAAAAAAAA8D8AAAAAAAAQwAAAAAAAAADwPwAAAAAAABTAywAAAAAAAAAAAAAAAAAAEMAAAAAAAAAA8D8AAAAAAAAYwAAAAAAAAADwPwAAAAAAABzAywAAAAAAAAAAAAAAAAAAGMDPAAAAAAAAAAAAAAAAAAAQwNMAAAAAAAAAAAAAAAAAAADAAAAAAAAAAPA/AAAAAAAAIMAAAAAAAAAA8D8AAAAAAAAiwMcAAAAAAAAAAAAAAAAAACDAAAAAAAAAAPA/AAAAAAAAJMAAAAAAAAAA8D8AAAAAAAAmwMcAAAAAAAAAAAAAAAAAACTAywAAAAAAAAAAAAAAAAAAIMAAAAAAAAAA8D8AAAAAAAAowAAAAAAAAADwPwAAAAAAACrAxwAAAAAAAAAAAAAAAAAAKMAAAAAAAAAA8D8AAAAAAAAswAAAAAAAAADwPwAAAAAAAC7AxwAAAAAAAAAAAAAAAAAALMDLAAAAAAAAAAAAAAAAAAAowM8AAAAAAAAAAAAAAAAAACDAAAAAAAAAAPA/AAAAAAAAMMAAAAAAAAAA8D8AAAAAAAAxwMMAAAAAAAAAAAAAAAAAADDAAAAAAAAAAPA/AAAAAAAAMsAAAAAAAAAA8D8AAAAAAAAzwMMAAAAAAAAAAAAAAAAAADLAxwAAAAAAAAAAAAAAAAAAMMAAAAAAAAAA8D8AAAAAAAA0wAAAAAAAAADwPwAAAAAAADXAwwAAAAAAAAAAAAAAAAAANMAAAAAAAAAA8D8AAAAAAAA2wAAAAAAAAADwPwAAAAAAADfAwwAAAAAAAAAAAAAAAAAANsDHAAAAAAAAAAAAAAAAAAA0wMsAAAAAAAAAAAAAAAAAADDAAAAAAAAAAPA/AAAAAAAAOMAAAAAAAAAA8D8AAAAAAAA5wMMAAAAAAAAAAAAAAAAAADjAAAAAAAAAAPA/AAAAAAAAOsAAAAAAAAAA8D8AAAAAAAA7wMMAAAAAAAAAAAAAAAAAADrAxwAAAAAAAAAAAAAAAAAAOMAAAAAAAAAA8D8AAAAAAAA8wAAAAAAAAADwPwAAAAAAAD3AwwAAAAAAAAAAAAAAAAAAPMAAAAAAAAAA8D8AAAAAAAA+wAAAAAAAAADwPwAAAAAAAD/AwwAAAAAAAAAAAAAAAAAAPsDHAAAAAAAAAAAAAAAAAAA8wMsAAAAAAAAAAAAAAAAAADjAzwAAAAAAAAAAAAAAAAAAMMDTAAAAAAAAAAAAAAAAAAAgwNcAAAAAAAAAAAAAAAAAAADAAAAAAAAAAPA/AAAAAAAAQMAAAAAAAAAA8D8AAAAAAIBAwL8AAAAAAAAAAAAAAAAAAEDAAAAAAAAAAPA/AAAAAAAAQcAAAAAAAAAA8D8AAAAAAIBBwL8AAAAAAAAAAAAAAAAAAEHAwwAAAAAAAAAAAAAAAAAAQMAAAAAAAAAA8D8AAAAAAABCwAAAAAAAAADwPwAAAAAAgELAvwAAAAAAAAAAAAAAAAAAQsAAAAAAAAAA8D8AAAAAAABDwAAAAAAAAADwPwAAAAAAgEPAvwAAAAAAAAAAAAAAAAAAQ8DDAAAAAAAAAAAAAAAAAABCwMcAAAAAAAAAAAAAAAAAAEDAAAAAAAAAAPA/AAAAAAAARMAAAAAAAAAA8D8AAAAAAIBEwL8AAAAAAAAAAAAAAAAAAETAAAAAAAAAAPA/AAAAAAAARcAAAAAAAAAA8D8AAAAAAIBFwL8AAAAAAAAAAAAAAAAAAEXAwwAAAAAAAAAAAAAAAAAARMAAAAAAAAAA8D8AAAAAAABGwAAAAAAAAADwPwAAAAAAgEbAvwAAAAAAAAAAAAAAAAAARsAAAAAAAAAA8D8AAAAAAABHwAAAAAAAAADwPwAAAAAAgEfAvwAAAAAAAAAAAAAAAAAAR8DDAAAAAAAAAAAAAAAAAABGwMcAAAAAAAAAAAAAAAAAAETAywAAAAAAAAAAAAAAAAAAQMAAAAAAAAAA8D8AAAAAAABIwAAAAAAAAADwPwAAAAAAgEjAvwAAAAAAAAAAAAAAAAAASMAAAAAAAAAA8D8AAAAAAABJwMMAAAAAAAAAAAAAAAAAAEjAzwAAAAAAAAAAAAAAAAAAQMDbAAAAAAAAAAAAAAAAAAAAwPsAAAAAAAAAAAAAAAAAAPC/";
  // QDigest: values 51 to 100
  const std::string kQDigest2 =
      "AK5H4XoUru8/AAAAAAAAAAAAAAAAAAAAAAAAAAAAgElAAAAAAAAAWUBjAAAAAAAAAAAAAPA/AAAAAACAScAAAAAAAAAA8D8AAAAAAABKwAAAAAAAAADwPwAAAAAAgErAvwAAAAAAAAAAAAAAAAAASsAAAAAAAAAA8D8AAAAAAABLwAAAAAAAAADwPwAAAAAAgEvAvwAAAAAAAAAAAAAAAAAAS8DDAAAAAAAAAAAAAAAAAABKwMcAAAAAAAAAAAAAAAAAgEnAAAAAAAAAAPA/AAAAAAAATMAAAAAAAAAA8D8AAAAAAIBMwL8AAAAAAAAAAAAAAAAAAEzAAAAAAAAAAPA/AAAAAAAATcAAAAAAAAAA8D8AAAAAAIBNwL8AAAAAAAAAAAAAAAAAAE3AwwAAAAAAAAAAAAAAAAAATMAAAAAAAAAA8D8AAAAAAABOwAAAAAAAAADwPwAAAAAAgE7AvwAAAAAAAAAAAAAAAAAATsAAAAAAAAAA8D8AAAAAAABPwAAAAAAAAADwPwAAAAAAgE/AvwAAAAAAAAAAAAAAAAAAT8DDAAAAAAAAAAAAAAAAAABOwMcAAAAAAAAAAAAAAAAAAEzAywAAAAAAAAAAAAAAAACAScAAAAAAAAAA8D8AAAAAAABQwAAAAAAAAADwPwAAAAAAQFDAuwAAAAAAAAAAAAAAAAAAUMAAAAAAAAAA8D8AAAAAAIBQwAAAAAAAAADwPwAAAAAAwFDAuwAAAAAAAAAAAAAAAACAUMC/AAAAAAAAAAAAAAAAAABQwAAAAAAAAADwPwAAAAAAAFHAAAAAAAAAAPA/AAAAAABAUcC7AAAAAAAAAAAAAAAAAABRwAAAAAAAAADwPwAAAAAAgFHAAAAAAAAAAPA/AAAAAADAUcC7AAAAAAAAAAAAAAAAAIBRwL8AAAAAAAAAAAAAAAAAAFHAwwAAAAAAAAAAAAAAAAAAUMAAAAAAAAAA8D8AAAAAAABSwAAAAAAAAADwPwAAAAAAQFLAuwAAAAAAAAAAAAAAAAAAUsAAAAAAAAAA8D8AAAAAAIBSwAAAAAAAAADwPwAAAAAAwFLAuwAAAAAAAAAAAAAAAACAUsC/AAAAAAAAAAAAAAAAAABSwAAAAAAAAADwPwAAAAAAAFPAAAAAAAAAAPA/AAAAAABAU8C7AAAAAAAAAAAAAAAAAABTwAAAAAAAAADwPwAAAAAAgFPAAAAAAAAAAPA/AAAAAADAU8C7AAAAAAAAAAAAAAAAAIBTwL8AAAAAAAAAAAAAAAAAAFPAwwAAAAAAAAAAAAAAAAAAUsDHAAAAAAAAAAAAAAAAAABQwAAAAAAAAADwPwAAAAAAAFTAAAAAAAAAAPA/AAAAAABAVMC7AAAAAAAAAAAAAAAAAABUwAAAAAAAAADwPwAAAAAAgFTAAAAAAAAAAPA/AAAAAADAVMC7AAAAAAAAAAAAAAAAAIBUwL8AAAAAAAAAAAAAAAAAAFTAAAAAAAAAAPA/AAAAAAAAVcAAAAAAAAAA8D8AAAAAAEBVwLsAAAAAAAAAAAAAAAAAAFXAAAAAAAAAAPA/AAAAAACAVcAAAAAAAAAA8D8AAAAAAMBVwLsAAAAAAAAAAAAAAAAAgFXAvwAAAAAAAAAAAAAAAAAAVcDDAAAAAAAAAAAAAAAAAABUwAAAAAAAAADwPwAAAAAAAFbAAAAAAAAAAPA/AAAAAABAVsC7AAAAAAAAAAAAAAAAAABWwAAAAAAAAADwPwAAAAAAgFbAAAAAAAAAAPA/AAAAAADAVsC7AAAAAAAAAAAAAAAAAIBWwL8AAAAAAAAAAAAAAAAAAFbAAAAAAAAAAPA/AAAAAAAAV8AAAAAAAAAA8D8AAAAAAEBXwLsAAAAAAAAAAAAAAAAAAFfAAAAAAAAAAPA/AAAAAACAV8AAAAAAAAAA8D8AAAAAAMBXwLsAAAAAAAAAAAAAAAAAgFfAvwAAAAAAAAAAAAAAAAAAV8DDAAAAAAAAAAAAAAAAAABWwMcAAAAAAAAAAAAAAAAAAFTAywAAAAAAAAAAAAAAAAAAUMAAAAAAAAAA8D8AAAAAAABYwAAAAAAAAADwPwAAAAAAQFjAuwAAAAAAAAAAAAAAAAAAWMAAAAAAAAAA8D8AAAAAAIBYwAAAAAAAAADwPwAAAAAAwFjAuwAAAAAAAAAAAAAAAACAWMC/AAAAAAAAAAAAAAAAAABYwAAAAAAAAADwPwAAAAAAAFnAwwAAAAAAAAAAAAAAAAAAWMDPAAAAAAAAAAAAAAAAAABQwNMAAAAAAAAAAAAAAAAAgEnA";

  // Create QDigest vectors from base64 strings
  auto qdigestData = makeFlatVector<std::string>({kQDigest1, kQDigest2});

  // Create plan and execute
  auto op =
      createMergePlan<double>(makeRowVector({qdigestData}), QDIGEST(DOUBLE()));
  auto result = readSingleValue(op);

  const std::string expectedMergedQDigest =
      "AK5H4XoUru8/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAPA/AAAAAAAAWUDHAAAAAAAAAAAAAPA/AAAAAAAA8L8AAAAAAAAA8D8AAAAAAAAAwAAAAAAAAADwPwAAAAAAAAjAzwAAAAAAAAAAAAAAAAAAAMAAAAAAAAAA8D8AAAAAAAAQwAAAAAAAAADwPwAAAAAAABTAywAAAAAAAAAAAAAAAAAAEMAAAAAAAAAA8D8AAAAAAAAYwAAAAAAAAADwPwAAAAAAABzAywAAAAAAAAAAAAAAAAAAGMDPAAAAAAAAAAAAAAAAAAAQwNMAAAAAAAAAAAAAAAAAAADAAAAAAAAAAPA/AAAAAAAAIMAAAAAAAAAA8D8AAAAAAAAiwMcAAAAAAAAAAAAAAAAAACDAAAAAAAAAAPA/AAAAAAAAJMAAAAAAAAAA8D8AAAAAAAAmwMcAAAAAAAAAAAAAAAAAACTAywAAAAAAAAAAAAAAAAAAIMAAAAAAAAAA8D8AAAAAAAAowAAAAAAAAADwPwAAAAAAACrAxwAAAAAAAAAAAAAAAAAAKMAAAAAAAAAA8D8AAAAAAAAswAAAAAAAAADwPwAAAAAAAC7AxwAAAAAAAAAAAAAAAAAALMDLAAAAAAAAAAAAAAAAAAAowM8AAAAAAAAAAAAAAAAAACDAAAAAAAAAAPA/AAAAAAAAMMAAAAAAAAAA8D8AAAAAAAAxwMMAAAAAAAAAAAAAAAAAADDAAAAAAAAAAPA/AAAAAAAAMsAAAAAAAAAA8D8AAAAAAAAzwMMAAAAAAAAAAAAAAAAAADLAxwAAAAAAAAAAAAAAAAAAMMAAAAAAAAAA8D8AAAAAAAA0wAAAAAAAAADwPwAAAAAAADXAwwAAAAAAAAAAAAAAAAAANMAAAAAAAAAA8D8AAAAAAAA2wAAAAAAAAADwPwAAAAAAADfAwwAAAAAAAAAAAAAAAAAANsDHAAAAAAAAAAAAAAAAAAA0wMsAAAAAAAAAAAAAAAAAADDAAAAAAAAAAPA/AAAAAAAAOMAAAAAAAAAA8D8AAAAAAAA5wMMAAAAAAAAAAAAAAAAAADjAAAAAAAAAAPA/AAAAAAAAOsAAAAAAAAAA8D8AAAAAAAA7wMMAAAAAAAAAAAAAAAAAADrAxwAAAAAAAAAAAAAAAAAAOMAAAAAAAAAA8D8AAAAAAAA8wAAAAAAAAADwPwAAAAAAAD3AwwAAAAAAAAAAAAAAAAAAPMAAAAAAAAAA8D8AAAAAAAA+wAAAAAAAAADwPwAAAAAAAD/AwwAAAAAAAAAAAAAAAAAAPsDHAAAAAAAAAAAAAAAAAAA8wMsAAAAAAAAAAAAAAAAAADjAzwAAAAAAAAAAAAAAAAAAMMDTAAAAAAAAAAAAAAAAAAAgwNcAAAAAAAAAAAAAAAAAAADAAAAAAAAAAPA/AAAAAAAAQMAAAAAAAAAA8D8AAAAAAIBAwL8AAAAAAAAAAAAAAAAAAEDAAAAAAAAAAPA/AAAAAAAAQcAAAAAAAAAA8D8AAAAAAIBBwL8AAAAAAAAAAAAAAAAAAEHAwwAAAAAAAAAAAAAAAAAAQMAAAAAAAAAA8D8AAAAAAABCwAAAAAAAAADwPwAAAAAAgELAvwAAAAAAAAAAAAAAAAAAQsAAAAAAAAAA8D8AAAAAAABDwAAAAAAAAADwPwAAAAAAgEPAvwAAAAAAAAAAAAAAAAAAQ8DDAAAAAAAAAAAAAAAAAABCwMcAAAAAAAAAAAAAAAAAAEDAAAAAAAAAAPA/AAAAAAAARMAAAAAAAAAA8D8AAAAAAIBEwL8AAAAAAAAAAAAAAAAAAETAAAAAAAAAAPA/AAAAAAAARcAAAAAAAAAA8D8AAAAAAIBFwL8AAAAAAAAAAAAAAAAAAEXAwwAAAAAAAAAAAAAAAAAARMAAAAAAAAAA8D8AAAAAAABGwAAAAAAAAADwPwAAAAAAgEbAvwAAAAAAAAAAAAAAAAAARsAAAAAAAAAA8D8AAAAAAABHwAAAAAAAAADwPwAAAAAAgEfAvwAAAAAAAAAAAAAAAAAAR8DDAAAAAAAAAAAAAAAAAABGwMcAAAAAAAAAAAAAAAAAAETAywAAAAAAAAAAAAAAAAAAQMAAAAAAAAAA8D8AAAAAAABIwAAAAAAAAADwPwAAAAAAgEjAvwAAAAAAAAAAAAAAAAAASMAAAAAAAAAA8D8AAAAAAABJwAAAAAAAAADwPwAAAAAAgEnAvwAAAAAAAAAAAAAAAAAAScDDAAAAAAAAAAAAAAAAAABIwAAAAAAAAADwPwAAAAAAAErAAAAAAAAAAPA/AAAAAACASsC/AAAAAAAAAAAAAAAAAABKwAAAAAAAAADwPwAAAAAAAEvAAAAAAAAAAPA/AAAAAACAS8C/AAAAAAAAAAAAAAAAAABLwMMAAAAAAAAAAAAAAAAAAErAxwAAAAAAAAAAAAAAAAAASMAAAAAAAAAA8D8AAAAAAABMwAAAAAAAAADwPwAAAAAAgEzAvwAAAAAAAAAAAAAAAAAATMAAAAAAAAAA8D8AAAAAAABNwAAAAAAAAADwPwAAAAAAgE3AvwAAAAAAAAAAAAAAAAAATcDDAAAAAAAAAAAAAAAAAABMwAAAAAAAAADwPwAAAAAAAE7AAAAAAAAAAPA/AAAAAACATsC/AAAAAAAAAAAAAAAAAABOwAAAAAAAAADwPwAAAAAAAE/AAAAAAAAAAPA/AAAAAACAT8C/AAAAAAAAAAAAAAAAAABPwMMAAAAAAAAAAAAAAAAAAE7AxwAAAAAAAAAAAAAAAAAATMDLAAAAAAAAAAAAAAAAAABIwM8AAAAAAAAAAAAAAAAAAEDAAAAAAAAAAPA/AAAAAAAAUMAAAAAAAAAA8D8AAAAAAEBQwLsAAAAAAAAAAAAAAAAAAFDAAAAAAAAAAPA/AAAAAACAUMAAAAAAAAAA8D8AAAAAAMBQwLsAAAAAAAAAAAAAAAAAgFDAvwAAAAAAAAAAAAAAAAAAUMAAAAAAAAAA8D8AAAAAAABRwAAAAAAAAADwPwAAAAAAQFHAuwAAAAAAAAAAAAAAAAAAUcAAAAAAAAAA8D8AAAAAAIBRwAAAAAAAAADwPwAAAAAAwFHAuwAAAAAAAAAAAAAAAACAUcC/AAAAAAAAAAAAAAAAAABRwMMAAAAAAAAAAAAAAAAAAFDAAAAAAAAAAPA/AAAAAAAAUsAAAAAAAAAA8D8AAAAAAEBSwLsAAAAAAAAAAAAAAAAAAFLAAAAAAAAAAPA/AAAAAACAUsAAAAAAAAAA8D8AAAAAAMBSwLsAAAAAAAAAAAAAAAAAgFLAvwAAAAAAAAAAAAAAAAAAUsAAAAAAAAAA8D8AAAAAAABTwAAAAAAAAADwPwAAAAAAQFPAuwAAAAAAAAAAAAAAAAAAU8AAAAAAAAAA8D8AAAAAAIBTwAAAAAAAAADwPwAAAAAAwFPAuwAAAAAAAAAAAAAAAACAU8C/AAAAAAAAAAAAAAAAAABTwMMAAAAAAAAAAAAAAAAAAFLAxwAAAAAAAAAAAAAAAAAAUMAAAAAAAAAA8D8AAAAAAABUwAAAAAAAAADwPwAAAAAAQFTAuwAAAAAAAAAAAAAAAAAAVMAAAAAAAAAA8D8AAAAAAIBUwAAAAAAAAADwPwAAAAAAwFTAuwAAAAAAAAAAAAAAAACAVMC/AAAAAAAAAAAAAAAAAABUwAAAAAAAAADwPwAAAAAAAFXAAAAAAAAAAPA/AAAAAABAVcC7AAAAAAAAAAAAAAAAAABVwAAAAAAAAADwPwAAAAAAgFXAAAAAAAAAAPA/AAAAAADAVcC7AAAAAAAAAAAAAAAAAIBVwL8AAAAAAAAAAAAAAAAAAFXAwwAAAAAAAAAAAAAAAAAAVMAAAAAAAAAA8D8AAAAAAABWwAAAAAAAAADwPwAAAAAAQFbAuwAAAAAAAAAAAAAAAAAAVsAAAAAAAAAA8D8AAAAAAIBWwAAAAAAAAADwPwAAAAAAwFbAuwAAAAAAAAAAAAAAAACAVsC/AAAAAAAAAAAAAAAAAABWwAAAAAAAAADwPwAAAAAAAFfAAAAAAAAAAPA/AAAAAABAV8C7AAAAAAAAAAAAAAAAAABXwAAAAAAAAADwPwAAAAAAgFfAAAAAAAAAAPA/AAAAAADAV8C7AAAAAAAAAAAAAAAAAIBXwL8AAAAAAAAAAAAAAAAAAFfAwwAAAAAAAAAAAAAAAAAAVsDHAAAAAAAAAAAAAAAAAABUwMsAAAAAAAAAAAAAAAAAAFDAAAAAAAAAAPA/AAAAAAAAWMAAAAAAAAAA8D8AAAAAAEBYwLsAAAAAAAAAAAAAAAAAAFjAAAAAAAAAAPA/AAAAAACAWMAAAAAAAAAA8D8AAAAAAMBYwLsAAAAAAAAAAAAAAAAAgFjAvwAAAAAAAAAAAAAAAAAAWMAAAAAAAAAA8D8AAAAAAABZwMMAAAAAAAAAAAAAAAAAAFjAzwAAAAAAAAAAAAAAAAAAUMDTAAAAAAAAAAAAAAAAAABAwNsAAAAAAAAAAAAAAAAAAADA+wAAAAAAAAAAAAAAAAAA8L8=";

  ASSERT_TRUE(qdigestEquals<double>(
      result.value<TypeKind::VARCHAR>(), expectedMergedQDigest));
}

TEST_F(MergeAggregateTest, mergeQDigestBigIntMatchJava) {
  registerQDigestType();
  // Base64-encoded QDigests from Presto Java
  // QDigest: values 1 to 50
  const std::string kQDigest1 =
      "AK5H4XoUru8/AAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAMgAAAAAAAAASAAAAAAAAAAAAABBABAAAAAAAAIAAAAAAAAAAEEAMAAAAAAAAgA8AAAAAAAAUQAEAAAAAAACAAAAAAAAAABBAEAAAAAAAAIAAAAAAAAAAEEAUAAAAAAAAgAsAAAAAAAAAABAAAAAAAACAAAAAAAAAAAhAGQAAAAAAAIAAAAAAAAAAEEAcAAAAAAAAgAsAAAAAAAAAABkAAAAAAACADwAAAAAAAAAAEAAAAAAAAIATAAAAAAAACEABAAAAAAAAgAAAAAAAAAAQQCAAAAAAAACAAAAAAAAAABBAJAAAAAAAAIALAAAAAAAAAAAgAAAAAAAAgA0AAAAAAAAUQCAAAAAAAACAAAAAAAAAAPA/MgAAAAAAAIATAAAAAAAAFEAgAAAAAAAAgBcAAAAAAAAAAAEAAAAAAACA";
  // QDigest: values 51 to 100
  const std::string kQDigest2 =
      "AK5H4XoUru8/AAAAAAAAAAAAAAAAAAAAADMAAAAAAAAAZAAAAAAAAAARAAAAAAAAAAAAABBAOAAAAAAAAIAAAAAAAAAAAEA+AAAAAAAAgAsAAAAAAAAAADgAAAAAAACADgAAAAAAABBAMwAAAAAAAIAAAAAAAAAAEEBEAAAAAAAAgAAAAAAAAAAQQEgAAAAAAACAAAAAAAAAABBATAAAAAAAAIALAAAAAAAAAABIAAAAAAAAgA8AAAAAAAAIQEAAAAAAAACAAAAAAAAAABBAUAAAAAAAAIAAAAAAAAAAEEBUAAAAAAAAgAsAAAAAAAAAAFAAAAAAAACAAAAAAAAAABRAWwAAAAAAAIAPAAAAAAAACEBQAAAAAAAAgBMAAAAAAAAAAEAAAAAAAACAFQAAAAAAABRAQAAAAAAAAIAbAAAAAAAAEEAzAAAAAAAAgA==";

  // Create QDigest vectors from base64 strings
  auto qdigestData = makeFlatVector<std::string>({kQDigest1, kQDigest2});

  // Create plan and execute
  auto op =
      createMergePlan<int64_t>(makeRowVector({qdigestData}), QDIGEST(BIGINT()));
  auto result = readSingleValue(op);

  // Expected merged QDigest base64 string from Java
  const std::string expectedMergedQDigest =
      "AK5H4XoUru8/AAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAZAAAAAAAAAATAAAAAAAAAAAAABBABAAAAAAAAIAAAAAAAAAAEEAMAAAAAAAAgA8AAAAAAAAUQAEAAAAAAACAAAAAAAAAACBAEAAAAAAAAIANAAAAAAAAHEAQAAAAAAAAgBMAAAAAAAAAAAEAAAAAAACAAAAAAAAAACBAIAAAAAAAAIANAAAAAAAAFEAgAAAAAAAAgAAAAAAAAAAcQDkAAAAAAACADgAAAAAAABxAMgAAAAAAAIATAAAAAAAAAAAgAAAAAAAAgBcAAAAAAAAAAAEAAAAAAACAAAAAAAAAACBAQAAAAAAAAIAAAAAAAAAAIEBIAAAAAAAAgA8AAAAAAAAAAEAAAAAAAACAEQAAAAAAABxAQAAAAAAAAIAAAAAAAAAAEEBhAAAAAAAAgBcAAAAAAAAiQEAAAAAAAACAGwAAAAAAACJAAQAAAAAAAIA=";

  ASSERT_TRUE(qdigestEquals<int64_t>(
      result.value<TypeKind::VARCHAR>(), expectedMergedQDigest));
}

TEST_F(MergeAggregateTest, mergeQDigestRealMatchJava) {
  registerQDigestType();
  // Base64-encoded QDigests from Presto Java
  // QDigest: values 1 to 50
  const std::string kQDigest1 =
      "AK5H4XoUru8/AAAAAAAAAAAAAAAAAAAAAAAAgD8AAAAAAABIQgAAAABjAAAAAAAAAAAAAPA/AACAPwAAAIAAAAAAAAAA8D8AAABAAAAAgAAAAAAAAADwPwAAQEAAAACAWwAAAAAAAAAAAAAAQAAAAIAAAAAAAAAA8D8AAIBAAAAAgAAAAAAAAADwPwAAoEAAAACAVwAAAAAAAAAAAACAQAAAAIAAAAAAAAAA8D8AAMBAAAAAgAAAAAAAAADwPwAA4EAAAACAVwAAAAAAAAAAAADAQAAAAIBbAAAAAAAAAAAAAIBAAAAAgF8AAAAAAAAAAAAAAEAAAACAAAAAAAAAAPA/AAAAQQAAAIAAAAAAAAAA8D8AABBBAAAAgFMAAAAAAAAAAAAAAEEAAACAAAAAAAAAAPA/AAAgQQAAAIAAAAAAAAAA8D8AADBBAAAAgFMAAAAAAAAAAAAAIEEAAACAVwAAAAAAAAAAAAAAQQAAAIAAAAAAAAAA8D8AAEBBAAAAgAAAAAAAAADwPwAAUEEAAACAUwAAAAAAAAAAAABAQQAAAIAAAAAAAAAA8D8AAGBBAAAAgAAAAAAAAADwPwAAcEEAAACAUwAAAAAAAAAAAABgQQAAAIBXAAAAAAAAAAAAAEBBAAAAgFsAAAAAAAAAAAAAAEEAAACAAAAAAAAAAPA/AACAQQAAAIAAAAAAAAAA8D8AAIhBAAAAgE8AAAAAAAAAAAAAgEEAAACAAAAAAAAAAPA/AACQQQAAAIAAAAAAAAAA8D8AAJhBAAAAgE8AAAAAAAAAAAAAkEEAAACAUwAAAAAAAAAAAACAQQAAAIAAAAAAAAAA8D8AAKBBAAAAgAAAAAAAAADwPwAAqEEAAACATwAAAAAAAAAAAACgQQAAAIAAAAAAAAAA8D8AALBBAAAAgAAAAAAAAADwPwAAuEEAAACATwAAAAAAAAAAAACwQQAAAIBTAAAAAAAAAAAAAKBBAAAAgFcAAAAAAAAAAAAAgEEAAACAAAAAAAAAAPA/AADAQQAAAIAAAAAAAAAA8D8AAMhBAAAAgE8AAAAAAAAAAAAAwEEAAACAAAAAAAAAAPA/AADQQQAAAIAAAAAAAAAA8D8AANhBAAAAgE8AAAAAAAAAAAAA0EEAAACAUwAAAAAAAAAAAADAQQAAAIAAAAAAAAAA8D8AAOBBAAAAgAAAAAAAAADwPwAA6EEAAACATwAAAAAAAAAAAADgQQAAAIAAAAAAAAAA8D8AAPBBAAAAgAAAAAAAAADwPwAA+EEAAACATwAAAAAAAAAAAADwQQAAAIBTAAAAAAAAAAAAAOBBAAAAgFcAAAAAAAAAAAAAwEEAAACAWwAAAAAAAAAAAACAQQAAAIBfAAAAAAAAAAAAAABBAAAAgGMAAAAAAAAAAAAAAEAAAACAAAAAAAAAAPA/AAAAQgAAAIAAAAAAAAAA8D8AAARCAAAAgEsAAAAAAAAAAAAAAEIAAACAAAAAAAAAAPA/AAAIQgAAAIAAAAAAAAAA8D8AAAxCAAAAgEsAAAAAAAAAAAAACEIAAACATwAAAAAAAAAAAAAAQgAAAIAAAAAAAAAA8D8AABBCAAAAgAAAAAAAAADwPwAAFEIAAACASwAAAAAAAAAAAAAQQgAAAIAAAAAAAAAA8D8AABhCAAAAgAAAAAAAAADwPwAAHEIAAACASwAAAAAAAAAAAAAYQgAAAIBPAAAAAAAAAAAAABBCAAAAgFMAAAAAAAAAAAAAAEIAAACAAAAAAAAAAPA/AAAgQgAAAIAAAAAAAAAA8D8AACRCAAAAgEsAAAAAAAAAAAAAIEIAAACAAAAAAAAAAPA/AAAoQgAAAIAAAAAAAAAA8D8AACxCAAAAgEsAAAAAAAAAAAAAKEIAAACATwAAAAAAAAAAAAAgQgAAAIAAAAAAAAAA8D8AADBCAAAAgAAAAAAAAADwPwAANEIAAACASwAAAAAAAAAAAAAwQgAAAIAAAAAAAAAA8D8AADhCAAAAgAAAAAAAAADwPwAAPEIAAACASwAAAAAAAAAAAAA4QgAAAIBPAAAAAAAAAAAAADBCAAAAgFMAAAAAAAAAAAAAIEIAAACAVwAAAAAAAAAAAAAAQgAAAIAAAAAAAAAA8D8AAEBCAAAAgAAAAAAAAADwPwAAREIAAACASwAAAAAAAAAAAABAQgAAAIAAAAAAAAAA8D8AAEhCAAAAgE8AAAAAAAAAAAAAQEIAAACAWwAAAAAAAAAAAAAAQgAAAIBnAAAAAAAAAAAAAABAAAAAgHsAAAAAAAAAAAAAgD8AAACA";
  // QDigest: values 51 to 100
  const std::string
      kQDigest2 =
          "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";

  // Create QDigest vectors from base64 strings
  auto qdigestData = makeFlatVector<std::string>({kQDigest1, kQDigest2});

  // Create plan and execute
  auto op =
      createMergePlan<float>(makeRowVector({qdigestData}), QDIGEST(REAL()));
  auto result = readSingleValue(op);

  // Expected merged QDigest base64 string from Java
  const std::string expectedMergedQDigest =
      "AK5H4XoUru8/AAAAAAAAAAAAAAAAAAAAAAAAgD8AAAAAAADIQgAAAABgAAAAVAAAAAAAAABAAACAQAAAAIBUAAAAAAAAAEAAAMBAAAAAgFsAAAAAAAAAAAAAgEAAAACAUAAAAAAAAABAAAAAQQAAAIBQAAAAAAAAAEAAACBBAAAAgFcAAAAAAAAAAAAAAEEAAACAUAAAAAAAAABAAABAQQAAAIBQAAAAAAAAAEAAAGBBAAAAgFcAAAAAAAAAAAAAQEEAAACAWwAAAAAAAAAAAAAAQQAAAIBMAAAAAAAAAEAAAIBBAAAAgEwAAAAAAAAAQAAAkEEAAACAUwAAAAAAAAAAAACAQQAAAIBMAAAAAAAAAEAAAKBBAAAAgEwAAAAAAAAAQAAAsEEAAACAUwAAAAAAAAAAAACgQQAAAIBXAAAAAAAAAAAAAIBBAAAAgEwAAAAAAAAAQAAAwEEAAACATAAAAAAAAABAAADQQQAAAIBTAAAAAAAAAAAAAMBBAAAAgEwAAAAAAAAAQAAA4EEAAACATAAAAAAAAABAAADwQQAAAIBTAAAAAAAAAAAAAOBBAAAAgFcAAAAAAAAAAAAAwEEAAACAWwAAAAAAAAAAAACAQQAAAIBfAAAAAAAAAAAAAABBAAAAgGMAAAAAAAAAQAAAAEAAAACASAAAAAAAAABAAAAAQgAAAIBIAAAAAAAAAEAAAAhCAAAAgE8AAAAAAAAAAAAAAEIAAACASAAAAAAAAABAAAAQQgAAAIBIAAAAAAAAAEAAABhCAAAAgE8AAAAAAAAAAAAAEEIAAACAUwAAAAAAAAAAAAAAQgAAAIBIAAAAAAAAAEAAACBCAAAAgEgAAAAAAAAAQAAAKEIAAACATwAAAAAAAAAAAAAgQgAAAIBIAAAAAAAAAEAAADBCAAAAgEgAAAAAAAAAQAAAOEIAAACATwAAAAAAAAAAAAAwQgAAAIBTAAAAAAAAAAAAACBCAAAAgFcAAAAAAAAAAAAAAEIAAACASAAAAAAAAABAAABAQgAAAIAAAAAAAAAA8D8AAEhCAAAAgE8AAAAAAAAAAAAAQEIAAACASAAAAAAAAABAAABQQgAAAIBIAAAAAAAAAEAAAFhCAAAAgE8AAAAAAAAAAAAAUEIAAACAUwAAAAAAAAAAAABAQgAAAIBIAAAAAAAAAEAAAGBCAAAAgEgAAAAAAAAAQAAAaEIAAACATwAAAAAAAAAAAABgQgAAAIBIAAAAAAAAAEAAAHBCAAAAgEgAAAAAAAAAQAAAeEIAAACATwAAAAAAAAAAAABwQgAAAIBTAAAAAAAAAAAAAGBCAAAAgFcAAAAAAAAAAAAATEIAAACAWwAAAAAAAAAAAAAAQgAAAIBEAAAAAAAAAEAAAIBCAAAAgEQAAAAAAAAAQAAAhEIAAACASwAAAAAAAAAAAACAQgAAAIBEAAAAAAAAAEAAAIhCAAAAgEQAAAAAAAAAQAAAjEIAAACASwAAAAAAAAAAAACIQgAAAIBPAAAAAAAAAAAAAIBCAAAAgEQAAAAAAAAAQAAAkEIAAACARAAAAAAAAABAAACUQgAAAIBLAAAAAAAAAAAAAJBCAAAAgEQAAAAAAAAAQAAAmEIAAACARAAAAAAAAABAAACcQgAAAIBLAAAAAAAAAAAAAJhCAAAAgE8AAAAAAAAAAAAAkEIAAACAUwAAAAAAAAAAAACAQgAAAIBEAAAAAAAAAEAAAKBCAAAAgEQAAAAAAAAAQAAApEIAAACASwAAAAAAAAAAAACgQgAAAIBEAAAAAAAAAEAAAKhCAAAAgEQAAAAAAAAAQAAArEIAAACASwAAAAAAAAAAAACoQgAAAIBPAAAAAAAAAAAAAKBCAAAAgEQAAAAAAAAAQAAAsEIAAACARAAAAAAAAABAAAC0QgAAAIBLAAAAAAAAAAAAALBCAAAAgEQAAAAAAAAAQAAAuEIAAACARAAAAAAAAABAAAC8QgAAAIBLAAAAAAAAAAAAALhCAAAAgE8AAAAAAAAAAAAAsEIAAACAUwAAAAAAAAAAAACgQgAAAIBXAAAAAAAAAAAAAIBCAAAAgEQAAAAAAAAAQAAAwEIAAACARAAAAAAAAABAAADEQgAAAIBLAAAAAAAAAAAAAMBCAAAAgFsAAAAAAAAAAAAAgEIAAACAXwAAAAAAAABAAABMQgAAAIBnAAAAAAAAAAAAAABAAAAAgHoAAAAAAADwPwAAgD8AAACA";

  ASSERT_TRUE(qdigestEquals<float>(
      result.value<TypeKind::VARCHAR>(), expectedMergedQDigest));
}

TEST_F(MergeAggregateTest, mergeQDigestOneNullMatchJava) {
  registerQDigestType();
  // Base64-encoded QDigests from Presto Java
  // QDigest: values 1 to 50
  const std::string kQDigest1 =
      "AK5H4XoUru8/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAPA/AAAAAAAASUBjAAAAAAAAAAAAAPA/AAAAAAAA8L8AAAAAAAAA8D8AAAAAAAAAwAAAAAAAAADwPwAAAAAAAAjAzwAAAAAAAAAAAAAAAAAAAMAAAAAAAAAA8D8AAAAAAAAQwAAAAAAAAADwPwAAAAAAABTAywAAAAAAAAAAAAAAAAAAEMAAAAAAAAAA8D8AAAAAAAAYwAAAAAAAAADwPwAAAAAAABzAywAAAAAAAAAAAAAAAAAAGMDPAAAAAAAAAAAAAAAAAAAQwNMAAAAAAAAAAAAAAAAAAADAAAAAAAAAAPA/AAAAAAAAIMAAAAAAAAAA8D8AAAAAAAAiwMcAAAAAAAAAAAAAAAAAACDAAAAAAAAAAPA/AAAAAAAAJMAAAAAAAAAA8D8AAAAAAAAmwMcAAAAAAAAAAAAAAAAAACTAywAAAAAAAAAAAAAAAAAAIMAAAAAAAAAA8D8AAAAAAAAowAAAAAAAAADwPwAAAAAAACrAxwAAAAAAAAAAAAAAAAAAKMAAAAAAAAAA8D8AAAAAAAAswAAAAAAAAADwPwAAAAAAAC7AxwAAAAAAAAAAAAAAAAAALMDLAAAAAAAAAAAAAAAAAAAowM8AAAAAAAAAAAAAAAAAACDAAAAAAAAAAPA/AAAAAAAAMMAAAAAAAAAA8D8AAAAAAAAxwMMAAAAAAAAAAAAAAAAAADDAAAAAAAAAAPA/AAAAAAAAMsAAAAAAAAAA8D8AAAAAAAAzwMMAAAAAAAAAAAAAAAAAADLAxwAAAAAAAAAAAAAAAAAAMMAAAAAAAAAA8D8AAAAAAAA0wAAAAAAAAADwPwAAAAAAADXAwwAAAAAAAAAAAAAAAAAANMAAAAAAAAAA8D8AAAAAAAA2wAAAAAAAAADwPwAAAAAAADfAwwAAAAAAAAAAAAAAAAAANsDHAAAAAAAAAAAAAAAAAAA0wMsAAAAAAAAAAAAAAAAAADDAAAAAAAAAAPA/AAAAAAAAOMAAAAAAAAAA8D8AAAAAAAA5wMMAAAAAAAAAAAAAAAAAADjAAAAAAAAAAPA/AAAAAAAAOsAAAAAAAAAA8D8AAAAAAAA7wMMAAAAAAAAAAAAAAAAAADrAxwAAAAAAAAAAAAAAAAAAOMAAAAAAAAAA8D8AAAAAAAA8wAAAAAAAAADwPwAAAAAAAD3AwwAAAAAAAAAAAAAAAAAAPMAAAAAAAAAA8D8AAAAAAAA+wAAAAAAAAADwPwAAAAAAAD/AwwAAAAAAAAAAAAAAAAAAPsDHAAAAAAAAAAAAAAAAAAA8wMsAAAAAAAAAAAAAAAAAADjAzwAAAAAAAAAAAAAAAAAAMMDTAAAAAAAAAAAAAAAAAAAgwNcAAAAAAAAAAAAAAAAAAADAAAAAAAAAAPA/AAAAAAAAQMAAAAAAAAAA8D8AAAAAAIBAwL8AAAAAAAAAAAAAAAAAAEDAAAAAAAAAAPA/AAAAAAAAQcAAAAAAAAAA8D8AAAAAAIBBwL8AAAAAAAAAAAAAAAAAAEHAwwAAAAAAAAAAAAAAAAAAQMAAAAAAAAAA8D8AAAAAAABCwAAAAAAAAADwPwAAAAAAgELAvwAAAAAAAAAAAAAAAAAAQsAAAAAAAAAA8D8AAAAAAABDwAAAAAAAAADwPwAAAAAAgEPAvwAAAAAAAAAAAAAAAAAAQ8DDAAAAAAAAAAAAAAAAAABCwMcAAAAAAAAAAAAAAAAAAEDAAAAAAAAAAPA/AAAAAAAARMAAAAAAAAAA8D8AAAAAAIBEwL8AAAAAAAAAAAAAAAAAAETAAAAAAAAAAPA/AAAAAAAARcAAAAAAAAAA8D8AAAAAAIBFwL8AAAAAAAAAAAAAAAAAAEXAwwAAAAAAAAAAAAAAAAAARMAAAAAAAAAA8D8AAAAAAABGwAAAAAAAAADwPwAAAAAAgEbAvwAAAAAAAAAAAAAAAAAARsAAAAAAAAAA8D8AAAAAAABHwAAAAAAAAADwPwAAAAAAgEfAvwAAAAAAAAAAAAAAAAAAR8DDAAAAAAAAAAAAAAAAAABGwMcAAAAAAAAAAAAAAAAAAETAywAAAAAAAAAAAAAAAAAAQMAAAAAAAAAA8D8AAAAAAABIwAAAAAAAAADwPwAAAAAAgEjAvwAAAAAAAAAAAAAAAAAASMAAAAAAAAAA8D8AAAAAAABJwMMAAAAAAAAAAAAAAAAAAEjAzwAAAAAAAAAAAAAAAAAAQMDbAAAAAAAAAAAAAAAAAAAAwPsAAAAAAAAAAAAAAAAAAPC/";
  // Empty QDigest string
  const std::string kQDigest2;

  // Create QDigest vectors from base64 strings
  auto qdigestData = makeFlatVector<std::string>({kQDigest1, kQDigest2});

  // Create plan and execute
  auto op =
      createMergePlan<int64_t>(makeRowVector({qdigestData}), QDIGEST(BIGINT()));
  auto result = readSingleValue(op);

  // When merging with a null QDigest, the result should be the same as the
  // original QDigest
  ASSERT_TRUE(
      qdigestEquals<float>(result.value<TypeKind::VARCHAR>(), kQDigest1));
}

TEST_F(MergeAggregateTest, mergeSfmSketch) {
  registerSfmSketchType();
  using SfmSketch = functions::aggregate::SfmSketch;
  HashStringAllocator allocator_{pool_.get()};

  // non-private sketch1: values [1, 2, 3]
  const std::string s1 =
      "BwkAAAAIAAAAAAAAAAAAAAA7AQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAg";
  // non-private sketch2: values [4, 5, 6, 7, 8]
  const std::string s2 =
      "BwkAAAAIAAAAAAAAAAAAAAB9AAAAAAABAAAAAAAAAIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgAAAAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACA=";
  const auto s1Binary = folly::base64Decode(s1);
  const auto s2Binary = folly::base64Decode(s2);

  auto vectors = makeRowVector({makeFlatVector<StringView>(
      {StringView(s1Binary), StringView(s2Binary)}, SFMSKETCH())});

  auto result1 =
      AssertQueryBuilder(PlanBuilder()
                             .values({vectors})
                             .singleAggregation({}, {"merge(c0)"}, {})
                             .planNode())
          .copyResults(pool());

  auto result2 =
      AssertQueryBuilder(PlanBuilder()
                             .values({vectors})
                             .partialAggregation({}, {"merge(c0)"}, {})
                             .finalAggregation()
                             .planNode())
          .copyResults(pool());

  const std::string expectedMerge =
      "BwkAAAAIAAAAAAAAAAAAAAA7AQAAAAABAAAAAAAAAIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgAgAAAAgQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAg";
  const auto expectedMergeBinary = folly::base64Decode(expectedMerge);

  ASSERT_EQ(result1->size(), 1);
  auto resultBinary1 =
      result1->childAt(0)->asFlatVector<StringView>()->valueAt(0);
  ASSERT_EQ(resultBinary1, StringView(expectedMergeBinary));
  ASSERT_EQ(
      SfmSketch::deserialize(resultBinary1.data(), &allocator_).cardinality(),
      8);

  ASSERT_EQ(result2->size(), 1);
  auto resultBinary2 =
      result2->childAt(0)->asFlatVector<StringView>()->valueAt(0);
  ASSERT_EQ(resultBinary2, StringView(expectedMergeBinary));
  ASSERT_EQ(
      SfmSketch::deserialize(resultBinary2.data(), &allocator_).cardinality(),
      8);
}
} // namespace
} // namespace facebook::velox::aggregate::test
