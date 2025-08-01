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

#include <folly/init/Init.h>
#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <unordered_set>

#include "velox/exec/fuzzer/WindowFuzzerRunner.h"
#include "velox/functions/prestosql/aggregates/RegisterAggregateFunctions.h"
#include "velox/functions/prestosql/fuzzer/ApproxDistinctInputGenerator.h"
#include "velox/functions/prestosql/fuzzer/ApproxDistinctResultVerifier.h"
#include "velox/functions/prestosql/fuzzer/ApproxPercentileInputGenerator.h"
#include "velox/functions/prestosql/fuzzer/ApproxPercentileResultVerifier.h"
#include "velox/functions/prestosql/fuzzer/AverageResultVerifier.h"
#include "velox/functions/prestosql/fuzzer/MinMaxInputGenerator.h"
#include "velox/functions/prestosql/fuzzer/QDigestAggInputGenerator.h"
#include "velox/functions/prestosql/fuzzer/QDigestAggResultVerifier.h"
#include "velox/functions/prestosql/fuzzer/TDigestAggregateInputGenerator.h"
#include "velox/functions/prestosql/fuzzer/TDigestAggregateResultVerifier.h"
#include "velox/functions/prestosql/fuzzer/WindowOffsetInputGenerator.h"
#include "velox/functions/prestosql/registration/RegistrationFunctions.h"
#include "velox/functions/prestosql/window/WindowFunctionsRegistration.h"
#include "velox/vector/fuzzer/VectorFuzzer.h"

DEFINE_int64(
    seed,
    0,
    "Initial seed for random number generator used to reproduce previous "
    "results (0 means start with random seed).");

DEFINE_string(
    only,
    "",
    "If specified, Fuzzer will only choose functions from "
    "this comma separated list of function names "
    "(e.g: --only \"min\" or --only \"sum,avg\").");

DEFINE_string(
    presto_url,
    "",
    "Presto coordinator URI along with port. If set, we use Presto "
    "source of truth. Otherwise, use DuckDB. Example: "
    "--presto_url=http://127.0.0.1:8080");

DEFINE_uint32(
    req_timeout_ms,
    1000,
    "Timeout in milliseconds for HTTP requests made to reference DB, "
    "such as Presto. Example: --req_timeout_ms=2000");

// Any change made in the file should be reflected in
// the FB-internal window fuzzer test too.
namespace facebook::velox::exec::test {
namespace {

std::unordered_map<std::string, std::shared_ptr<InputGenerator>>
getCustomInputGenerators() {
  return {
      {"min", std::make_shared<MinMaxInputGenerator>("min")},
      {"min_by", std::make_shared<MinMaxInputGenerator>("min_by")},
      {"max", std::make_shared<MinMaxInputGenerator>("max")},
      {"max_by", std::make_shared<MinMaxInputGenerator>("max_by")},
      {"approx_distinct", std::make_shared<ApproxDistinctInputGenerator>()},
      {"approx_set", std::make_shared<ApproxDistinctInputGenerator>()},
      {"approx_percentile", std::make_shared<ApproxPercentileInputGenerator>()},
      {"tdigest_agg", std::make_shared<TDigestAggregateInputGenerator>()},
      {"qdigest_agg", std::make_shared<QDigestAggInputGenerator>()},
      {"lead", std::make_shared<WindowOffsetInputGenerator>(1)},
      {"lag", std::make_shared<WindowOffsetInputGenerator>(1)},
      {"nth_value", std::make_shared<WindowOffsetInputGenerator>(1)},
      {"ntile", std::make_shared<WindowOffsetInputGenerator>(0)}};
}

} // namespace
} // namespace facebook::velox::exec::test

int main(int argc, char** argv) {
  facebook::velox::aggregate::prestosql::registerAllAggregateFunctions(
      "", false, true);
  facebook::velox::aggregate::prestosql::registerInternalAggregateFunctions("");
  facebook::velox::window::prestosql::registerAllWindowFunctions();
  facebook::velox::functions::prestosql::registerAllScalarFunctions();
  facebook::velox::memory::MemoryManager::initialize(
      facebook::velox::memory::MemoryManager::Options{});

  ::testing::InitGoogleTest(&argc, argv);

  // Calls common init functions in the necessary order, initializing
  // singletons, installing proper signal handlers for better debugging
  // experience, and initialize glog and gflags.
  folly::Init init(&argc, &argv);

  size_t initialSeed = FLAGS_seed == 0 ? std::time(nullptr) : FLAGS_seed;

  // List of functions that have known bugs that cause crashes or failures.
  std::unordered_set<std::string> skipFunctions = {
      // https://github.com/prestodb/presto/issues/24936
      "classification_fall_out",
      "classification_precision",
      "classification_recall",
      "classification_miss_rate",
      "classification_thresholds",
      // Skip internal functions used only for result verifications.
      "$internal$count_distinct",
      "$internal$array_agg",
      // https://github.com/facebookincubator/velox/issues/3493
      "stddev_pop",
      // Lambda functions are not supported yet.
      "reduce_agg",
      // array_agg requires a flag controlling whether to ignore nulls.
      "array_agg",
      // Skip non-deterministic functions.
      "noisy_avg_gaussian",
      "noisy_count_if_gaussian",
      "noisy_count_gaussian",
      "noisy_sum_gaussian",
      "noisy_approx_set_sfm",
      "noisy_approx_distinct_sfm",
      "noisy_approx_set_sfm_from_index_and_zeros",
      // https://github.com/facebookincubator/velox/issues/13547
      "merge",
  };

  if (!FLAGS_presto_url.empty()) {
    skipFunctions.insert({
        // min_by and max_by with 3 arguments produces results with different
        // orders of elements from Presto.
        "min_by",
        "max_by",
    });
  }

  // Functions whose results verification should be skipped. These can be
  // functions that return complex-typed results containing floating-point
  // fields.
  // TODO: allow custom result verifiers.
  using facebook::velox::exec::test::ApproxDistinctResultVerifier;
  using facebook::velox::exec::test::ApproxPercentileResultVerifier;
  using facebook::velox::exec::test::AverageResultVerifier;
  using facebook::velox::exec::test::QDigestAggResultVerifier;
  using facebook::velox::exec::test::TDigestAggregateResultVerifier;

  static const std::unordered_map<
      std::string,
      std::shared_ptr<facebook::velox::exec::test::ResultVerifier>>
      customVerificationFunctions = {
          // Approx functions.
          {"approx_distinct", std::make_shared<ApproxDistinctResultVerifier>()},
          {"approx_set", std::make_shared<ApproxDistinctResultVerifier>(true)},
          {"approx_percentile",
           std::make_shared<ApproxPercentileResultVerifier>()},
          {"approx_most_frequent", nullptr},
          {"tdigest_agg", std::make_shared<TDigestAggregateResultVerifier>()},
          {"qdigest_agg", std::make_shared<QDigestAggResultVerifier>()},
          {"merge", nullptr},
          // Semantically inconsistent functions
          {"skewness", nullptr},
          {"kurtosis", nullptr},
          {"entropy", nullptr},
          // https://github.com/facebookincubator/velox/issues/6330
          {"max_data_size_for_stats", nullptr},
          {"sum_data_size_for_stats", nullptr},
          {"avg", std::make_shared<AverageResultVerifier>()},
      };

  static const std::unordered_set<std::string> orderDependentFunctions = {
      // Window functions.
      "first_value",
      "last_value",
      "nth_value",
      "ntile",
      "lag",
      "lead",
      "row_number",
      "cume_dist",
      "rank",
      "dense_rank",
      "percent_rank",
      // Aggregation functions.
      "any_value",
      "arbitrary",
      "array_agg",
      "set_agg",
      "set_union",
      "map_agg",
      "map_union",
      "map_union_sum",
      "max_by",
      "min_by",
      "multimap_agg",
      "tdigest_agg",
      "qdigest_agg",
  };

  using Runner = facebook::velox::exec::test::WindowFuzzerRunner;
  using Options = facebook::velox::exec::test::AggregationFuzzerOptions;
  using facebook::velox::exec::test::setupReferenceQueryRunner;

  Options options;
  options.onlyFunctions = FLAGS_only;
  options.skipFunctions = skipFunctions;
  options.customVerificationFunctions = customVerificationFunctions;
  options.customInputGenerators =
      facebook::velox::exec::test::getCustomInputGenerators();
  options.orderDependentFunctions = orderDependentFunctions;
  options.timestampPrecision =
      facebook::velox::VectorFuzzer::Options::TimestampPrecision::kMilliSeconds;
  std::shared_ptr<facebook::velox::memory::MemoryPool> rootPool{
      facebook::velox::memory::memoryManager()->addRootPool()};
  return Runner::run(
      initialSeed,
      setupReferenceQueryRunner(
          rootPool.get(),
          FLAGS_presto_url,
          "window_fuzzer",
          FLAGS_req_timeout_ms),
      options);
}
