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

#include "velox/exec/tests/utils/TableWriterTestBase.h"

#include "folly/dynamic.h"
#include "velox/common/base/Fs.h"
#include "velox/common/base/tests/GTestUtils.h"
#include "velox/common/hyperloglog/SparseHll.h"
#include "velox/common/testutil/TestValue.h"
#include "velox/connectors/hive/HiveConfig.h"
#include "velox/connectors/hive/HivePartitionFunction.h"
#include "velox/dwio/common/WriterFactory.h"
#include "velox/exec/PlanNodeStats.h"
#include "velox/exec/TableWriter.h"
#include "velox/exec/tests/utils/AssertQueryBuilder.h"
#include "velox/exec/tests/utils/HiveConnectorTestBase.h"
#include "velox/exec/tests/utils/PlanBuilder.h"
#include "velox/exec/tests/utils/TempDirectoryPath.h"
#include "velox/vector/fuzzer/VectorFuzzer.h"

#include <re2/re2.h>
#include <string>
#include "folly/experimental/EventCount.h"
#include "velox/common/memory/MemoryArbitrator.h"
#include "velox/dwio/common/Options.h"
#include "velox/dwio/dwrf/writer/Writer.h"
#include "velox/exec/tests/utils/ArbitratorTestUtil.h"

namespace velox::exec::test {
constexpr uint64_t kQueryMemoryCapacity = 512 * MB;

class BasicTableWriterTestBase : public HiveConnectorTestBase {};

TEST_F(BasicTableWriterTestBase, roundTrip) {
  vector_size_t size = 1'000;
  auto data = makeRowVector({
      makeFlatVector<int32_t>(size, [](auto row) { return row; }),
      makeFlatVector<int32_t>(
          size, [](auto row) { return row * 2; }, nullEvery(7)),
  });

  auto sourceFilePath = TempFilePath::create();
  writeToFile(sourceFilePath->getPath(), data);

  auto targetDirectoryPath = TempDirectoryPath::create();

  auto rowType = asRowType(data->type());
  auto plan = PlanBuilder()
                  .tableScan(rowType)
                  .tableWrite(targetDirectoryPath->getPath())
                  .planNode();

  auto results = AssertQueryBuilder(plan)
                     .split(makeHiveConnectorSplit(sourceFilePath->getPath()))
                     .copyResults(pool());
  ASSERT_EQ(2, results->size());

  // First column has number of rows written in the first row and nulls in other
  // rows.
  auto rowCount = results->childAt(TableWriteTraits::kRowCountChannel)
                      ->as<FlatVector<int64_t>>();
  ASSERT_FALSE(rowCount->isNullAt(0));
  ASSERT_EQ(size, rowCount->valueAt(0));
  ASSERT_TRUE(rowCount->isNullAt(1));

  // Second column contains details about written files.
  auto details = results->childAt(TableWriteTraits::kFragmentChannel)
                     ->as<FlatVector<StringView>>();
  ASSERT_TRUE(details->isNullAt(0));
  ASSERT_FALSE(details->isNullAt(1));
  folly::dynamic obj = folly::parseJson(details->valueAt(1));

  ASSERT_EQ(size, obj["rowCount"].asInt());
  auto fileWriteInfos = obj["fileWriteInfos"];
  ASSERT_EQ(1, fileWriteInfos.size());

  auto writeFileName = fileWriteInfos[0]["writeFileName"].asString();

  // Read from 'writeFileName' and verify the data matches the original.
  plan = PlanBuilder().tableScan(rowType).planNode();

  auto copy = AssertQueryBuilder(plan)
                  .split(makeHiveConnectorSplit(fmt::format(
                      "{}/{}", targetDirectoryPath->getPath(), writeFileName)))
                  .copyResults(pool());
  assertEqualResults({data}, {copy});
}

// Generates a struct (row), write it as a flap map, and check that it is read
// back as a map.
TEST_F(BasicTableWriterTestBase, structAsMap) {
  // Input struct type.
  vector_size_t size = 1'000;
  auto data = makeRowVector(
      {"col1"},
      {
          makeRowVector(
              // Struct field names are the feature/map keys.
              {"1", "2"},
              {
                  makeFlatVector<int32_t>(size, [](auto row) { return row; }),
                  makeFlatVector<int32_t>(size, [](auto row) { return row; }),
              }),
      });

  // Write it as a flat map.
  auto outputType = ROW({"col1"}, {MAP(INTEGER(), INTEGER())});
  auto targetDirectoryPath = TempDirectoryPath::create();
  std::string fileName = "output_file";

  auto plan = PlanBuilder()
                  .values({data})
                  .tableWrite(
                      targetDirectoryPath->getPath(),
                      {},
                      0,
                      {},
                      {},
                      dwio::common::FileFormat::DWRF,
                      {},
                      PlanBuilder::kHiveDefaultConnectorId,
                      {
                          {"orc.flatten.map", "true"},
                          {"orc.map.flat.cols", "0"},
                          {"orc.map.flat.cols.struct.keys", "[[\"1\", \"2\"]]"},
                      },
                      nullptr,
                      fileName,
                      common::CompressionKind_NONE,
                      outputType)
                  .planNode();
  auto writerResults = AssertQueryBuilder(plan).copyResults(pool());

  // Check we get the expected map after reading.
  auto expected = makeRowVector(
      {"col1"},
      {
          makeMapVector<int32_t, int32_t>(
              size,
              [](auto /*row*/) { return 2; },
              [](auto row) { return row % 2 == 0 ? 2 : 1; },
              [](auto row) { return row / 2; }),
      });
  plan = PlanBuilder().tableScan(outputType).planNode();
  AssertQueryBuilder(plan)
      .split(makeHiveConnectorSplit(
          targetDirectoryPath->getPath() + "/" + fileName))
      .assertResults(expected);
}

TEST_F(BasicTableWriterTestBase, targetFileName) {
  constexpr const char* kFileName = "test.dwrf";
  auto data = makeRowVector({makeFlatVector<int64_t>(10, folly::identity)});
  auto directory = TempDirectoryPath::create();
  auto plan = PlanBuilder()
                  .values({data})
                  .tableWrite(
                      directory->getPath(),
                      dwio::common::FileFormat::DWRF,
                      {},
                      nullptr,
                      kFileName)
                  .planNode();
  auto results = AssertQueryBuilder(plan).copyResults(pool());
  auto* details = results->childAt(TableWriteTraits::kFragmentChannel)
                      ->asUnchecked<SimpleVector<StringView>>();
  auto detail = folly::parseJson(details->valueAt(1));
  auto fileWriteInfos = detail["fileWriteInfos"];
  ASSERT_EQ(1, fileWriteInfos.size());
  ASSERT_EQ(fileWriteInfos[0]["writeFileName"].asString(), kFileName);
  plan = PlanBuilder().tableScan(asRowType(data->type())).planNode();
  AssertQueryBuilder(plan)
      .split(makeHiveConnectorSplit(
          fmt::format("{}/{}", directory->getPath(), kFileName)))
      .assertResults(data);
}

class PartitionedTableWriterTest
    : public TableWriterTestBase,
      public testing::WithParamInterface<uint64_t> {
 public:
  PartitionedTableWriterTest() : TableWriterTestBase(GetParam()) {}

  static std::vector<uint64_t> getTestParams() {
    std::vector<uint64_t> testParams;
    const std::vector<bool> multiDriverOptions = {false, true};
    std::vector<FileFormat> fileFormats = {FileFormat::DWRF};
    if (hasWriterFactory(FileFormat::PARQUET)) {
      fileFormats.push_back(FileFormat::PARQUET);
    }
    for (bool multiDrivers : multiDriverOptions) {
      for (FileFormat fileFormat : fileFormats) {
        for (bool scaleWriter : {false, true}) {
          testParams.push_back(TestParam{
              fileFormat,
              TestMode::kPartitioned,
              CommitStrategy::kNoCommit,
              HiveBucketProperty::Kind::kHiveCompatible,
              false,
              multiDrivers,
              CompressionKind_ZSTD,
              scaleWriter}
                                   .value);
          testParams.push_back(TestParam{
              fileFormat,
              TestMode::kPartitioned,
              CommitStrategy::kTaskCommit,
              HiveBucketProperty::Kind::kHiveCompatible,
              false,
              multiDrivers,
              CompressionKind_ZSTD,
              scaleWriter}
                                   .value);
          testParams.push_back(TestParam{
              fileFormat,
              TestMode::kBucketed,
              CommitStrategy::kNoCommit,
              HiveBucketProperty::Kind::kHiveCompatible,
              false,
              multiDrivers,
              CompressionKind_ZSTD,
              scaleWriter}
                                   .value);
          testParams.push_back(TestParam{
              fileFormat,
              TestMode::kBucketed,
              CommitStrategy::kTaskCommit,
              HiveBucketProperty::Kind::kHiveCompatible,
              false,
              multiDrivers,
              CompressionKind_ZSTD,
              scaleWriter}
                                   .value);
          testParams.push_back(TestParam{
              fileFormat,
              TestMode::kBucketed,
              CommitStrategy::kNoCommit,
              HiveBucketProperty::Kind::kPrestoNative,
              false,
              multiDrivers,
              CompressionKind_ZSTD,
              scaleWriter}
                                   .value);
          testParams.push_back(TestParam{
              fileFormat,
              TestMode::kBucketed,
              CommitStrategy::kTaskCommit,
              HiveBucketProperty::Kind::kPrestoNative,
              false,
              multiDrivers,
              CompressionKind_ZSTD,
              scaleWriter}
                                   .value);
        }
      }
    }
    return testParams;
  }
};

class UnpartitionedTableWriterTest
    : public TableWriterTestBase,
      public testing::WithParamInterface<uint64_t> {
 public:
  UnpartitionedTableWriterTest() : TableWriterTestBase(GetParam()) {}

  static std::vector<uint64_t> getTestParams() {
    std::vector<uint64_t> testParams;
    const std::vector<bool> multiDriverOptions = {false, true};
    std::vector<FileFormat> fileFormats = {FileFormat::DWRF};
    if (hasWriterFactory(FileFormat::PARQUET)) {
      fileFormats.push_back(FileFormat::PARQUET);
    }
    for (bool multiDrivers : multiDriverOptions) {
      for (FileFormat fileFormat : fileFormats) {
        for (bool scaleWriter : {false, true}) {
          testParams.push_back(TestParam{
              fileFormat,
              TestMode::kUnpartitioned,
              CommitStrategy::kNoCommit,
              HiveBucketProperty::Kind::kHiveCompatible,
              false,
              multiDrivers,
              CompressionKind_NONE,
              scaleWriter}
                                   .value);
          testParams.push_back(TestParam{
              fileFormat,
              TestMode::kUnpartitioned,
              CommitStrategy::kTaskCommit,
              HiveBucketProperty::Kind::kHiveCompatible,
              false,
              multiDrivers,
              CompressionKind_NONE,
              scaleWriter}
                                   .value);
        }
      }
    }
    return testParams;
  }
};

class BucketedUnpartitionedTableWriterTest
    : public TableWriterTestBase,
      public testing::WithParamInterface<uint64_t> {
 public:
  BucketedUnpartitionedTableWriterTest() : TableWriterTestBase(GetParam()) {}

  static std::vector<uint64_t> getTestParams() {
    std::vector<uint64_t> testParams;
    const std::vector<bool> multiDriverOptions = {false, true};
    std::vector<FileFormat> fileFormats = {FileFormat::DWRF};
    if (hasWriterFactory(FileFormat::PARQUET)) {
      fileFormats.push_back(FileFormat::PARQUET);
    }
    const std::vector<TestMode> bucketModes = {TestMode::kOnlyBucketed};
    for (bool multiDrivers : multiDriverOptions) {
      for (FileFormat fileFormat : fileFormats) {
        testParams.push_back(TestParam{
            fileFormat,
            TestMode::kOnlyBucketed,
            CommitStrategy::kNoCommit,
            HiveBucketProperty::Kind::kHiveCompatible,
            true,
            multiDrivers,
            facebook::velox::common::CompressionKind_ZSTD,
            /*scaleWriter=*/false}
                                 .value);
        testParams.push_back(TestParam{
            fileFormat,
            TestMode::kOnlyBucketed,
            CommitStrategy::kTaskCommit,
            HiveBucketProperty::Kind::kHiveCompatible,
            true,
            multiDrivers,
            facebook::velox::common::CompressionKind_NONE,
            /*scaleWriter=*/false}
                                 .value);
      }
    }
    return testParams;
  }
};

class BucketedTableOnlyWriteTest
    : public TableWriterTestBase,
      public testing::WithParamInterface<uint64_t> {
 public:
  BucketedTableOnlyWriteTest() : TableWriterTestBase(GetParam()) {}

  static std::vector<uint64_t> getTestParams() {
    std::vector<uint64_t> testParams;
    const std::vector<bool> multiDriverOptions = {false, true};
    std::vector<FileFormat> fileFormats = {FileFormat::DWRF};
    if (hasWriterFactory(FileFormat::PARQUET)) {
      fileFormats.push_back(FileFormat::PARQUET);
    }
    const std::vector<TestMode> bucketModes = {
        TestMode::kBucketed, TestMode::kOnlyBucketed};
    for (bool multiDrivers : multiDriverOptions) {
      for (FileFormat fileFormat : fileFormats) {
        for (auto bucketMode : bucketModes) {
          testParams.push_back(TestParam{
              fileFormat,
              bucketMode,
              CommitStrategy::kNoCommit,
              HiveBucketProperty::Kind::kHiveCompatible,
              false,
              multiDrivers,
              CompressionKind_ZSTD,
              /*scaleWriter=*/false}
                                   .value);
          testParams.push_back(TestParam{
              fileFormat,
              bucketMode,
              CommitStrategy::kNoCommit,
              HiveBucketProperty::Kind::kHiveCompatible,
              true,
              multiDrivers,
              CompressionKind_ZSTD,
              /*scaleWriter=*/false}
                                   .value);
          testParams.push_back(TestParam{
              fileFormat,
              bucketMode,
              CommitStrategy::kTaskCommit,
              HiveBucketProperty::Kind::kHiveCompatible,
              false,
              multiDrivers,
              CompressionKind_ZSTD,
              /*scaleWriter=*/false}
                                   .value);
          testParams.push_back(TestParam{
              fileFormat,
              bucketMode,
              CommitStrategy::kTaskCommit,
              HiveBucketProperty::Kind::kHiveCompatible,
              true,
              multiDrivers,
              CompressionKind_ZSTD,
              /*scaleWriter=*/false}
                                   .value);
          testParams.push_back(TestParam{
              fileFormat,
              bucketMode,
              CommitStrategy::kNoCommit,
              HiveBucketProperty::Kind::kPrestoNative,
              false,
              multiDrivers,
              CompressionKind_ZSTD,
              /*scaleWriter=*/false}
                                   .value);
          testParams.push_back(TestParam{
              fileFormat,
              bucketMode,
              CommitStrategy::kNoCommit,
              HiveBucketProperty::Kind::kPrestoNative,
              true,
              multiDrivers,
              CompressionKind_ZSTD,
              /*scaleWriter=*/false}
                                   .value);
          testParams.push_back(TestParam{
              fileFormat,
              bucketMode,
              CommitStrategy::kTaskCommit,
              HiveBucketProperty::Kind::kPrestoNative,
              false,
              multiDrivers,
              CompressionKind_ZSTD,
              /*scaleWriter=*/false}
                                   .value);
          testParams.push_back(TestParam{
              fileFormat,
              bucketMode,
              CommitStrategy::kNoCommit,
              HiveBucketProperty::Kind::kPrestoNative,
              true,
              multiDrivers,
              CompressionKind_ZSTD,
              /*scaleWriter=*/false}
                                   .value);
        }
      }
    }
    return testParams;
  }
};

class BucketSortOnlyTableWriterTest
    : public TableWriterTestBase,
      public testing::WithParamInterface<uint64_t> {
 public:
  BucketSortOnlyTableWriterTest() : TableWriterTestBase(GetParam()) {}

  static std::vector<uint64_t> getTestParams() {
    std::vector<uint64_t> testParams;
    const std::vector<bool> multiDriverOptions = {false, true};
    std::vector<FileFormat> fileFormats = {FileFormat::DWRF};
    if (hasWriterFactory(FileFormat::PARQUET)) {
      fileFormats.push_back(FileFormat::PARQUET);
    }
    const std::vector<TestMode> bucketModes = {
        TestMode::kBucketed, TestMode::kOnlyBucketed};
    for (bool multiDrivers : multiDriverOptions) {
      for (FileFormat fileFormat : fileFormats) {
        for (auto bucketMode : bucketModes) {
          testParams.push_back(TestParam{
              fileFormat,
              bucketMode,
              CommitStrategy::kNoCommit,
              HiveBucketProperty::Kind::kHiveCompatible,
              true,
              multiDrivers,
              facebook::velox::common::CompressionKind_ZSTD,
              /*scaleWriter=*/false}
                                   .value);
          testParams.push_back(TestParam{
              fileFormat,
              bucketMode,
              CommitStrategy::kTaskCommit,
              HiveBucketProperty::Kind::kHiveCompatible,
              true,
              multiDrivers,
              facebook::velox::common::CompressionKind_NONE,
              /*scaleWriter=*/false}
                                   .value);
        }
      }
    }
    return testParams;
  }
};

class PartitionedWithoutBucketTableWriterTest
    : public TableWriterTestBase,
      public testing::WithParamInterface<uint64_t> {
 public:
  PartitionedWithoutBucketTableWriterTest() : TableWriterTestBase(GetParam()) {}

  static std::vector<uint64_t> getTestParams() {
    std::vector<uint64_t> testParams;
    const std::vector<bool> multiDriverOptions = {false, true};
    std::vector<FileFormat> fileFormats = {FileFormat::DWRF};
    if (hasWriterFactory(FileFormat::PARQUET)) {
      fileFormats.push_back(FileFormat::PARQUET);
    }
    for (bool multiDrivers : multiDriverOptions) {
      for (FileFormat fileFormat : fileFormats) {
        for (bool scaleWriter : {false, true}) {
          testParams.push_back(TestParam{
              fileFormat,
              TestMode::kPartitioned,
              CommitStrategy::kNoCommit,
              HiveBucketProperty::Kind::kHiveCompatible,
              false,
              multiDrivers,
              CompressionKind_ZSTD,
              scaleWriter}
                                   .value);
          testParams.push_back(TestParam{
              fileFormat,
              TestMode::kPartitioned,
              CommitStrategy::kTaskCommit,
              HiveBucketProperty::Kind::kHiveCompatible,
              false,
              true,
              CompressionKind_ZSTD,
              scaleWriter}
                                   .value);
        }
      }
    }
    return testParams;
  }
};

class AllTableWriterTest : public TableWriterTestBase,
                           public testing::WithParamInterface<uint64_t> {
 public:
  AllTableWriterTest() : TableWriterTestBase(GetParam()) {}

  static std::vector<uint64_t> getTestParams() {
    std::vector<uint64_t> testParams;
    const std::vector<bool> multiDriverOptions = {false, true};
    std::vector<FileFormat> fileFormats = {FileFormat::DWRF};
    if (hasWriterFactory(FileFormat::PARQUET)) {
      fileFormats.push_back(FileFormat::PARQUET);
    }
    for (bool multiDrivers : multiDriverOptions) {
      for (FileFormat fileFormat : fileFormats) {
        for (bool scaleWriter : {false, true}) {
          testParams.push_back(TestParam{
              fileFormat,
              TestMode::kUnpartitioned,
              CommitStrategy::kNoCommit,
              HiveBucketProperty::Kind::kHiveCompatible,
              false,
              multiDrivers,
              CompressionKind_ZSTD,
              scaleWriter}
                                   .value);
          testParams.push_back(TestParam{
              fileFormat,
              TestMode::kUnpartitioned,
              CommitStrategy::kTaskCommit,
              HiveBucketProperty::Kind::kHiveCompatible,
              false,
              multiDrivers,
              CompressionKind_ZSTD,
              scaleWriter}
                                   .value);
          testParams.push_back(TestParam{
              fileFormat,
              TestMode::kPartitioned,
              CommitStrategy::kNoCommit,
              HiveBucketProperty::Kind::kHiveCompatible,
              false,
              multiDrivers,
              CompressionKind_ZSTD,
              scaleWriter}
                                   .value);
          testParams.push_back(TestParam{
              fileFormat,
              TestMode::kPartitioned,
              CommitStrategy::kTaskCommit,
              HiveBucketProperty::Kind::kHiveCompatible,
              false,
              multiDrivers,
              CompressionKind_ZSTD,
              scaleWriter}
                                   .value);
          testParams.push_back(TestParam{
              fileFormat,
              TestMode::kBucketed,
              CommitStrategy::kNoCommit,
              HiveBucketProperty::Kind::kHiveCompatible,
              false,
              multiDrivers,
              CompressionKind_ZSTD,
              scaleWriter}
                                   .value);
          testParams.push_back(TestParam{
              fileFormat,
              TestMode::kBucketed,
              CommitStrategy::kTaskCommit,
              HiveBucketProperty::Kind::kHiveCompatible,
              false,
              multiDrivers,
              CompressionKind_ZSTD,
              scaleWriter}
                                   .value);
          testParams.push_back(TestParam{
              fileFormat,
              TestMode::kBucketed,
              CommitStrategy::kNoCommit,
              HiveBucketProperty::Kind::kPrestoNative,
              false,
              multiDrivers,
              CompressionKind_ZSTD,
              scaleWriter}
                                   .value);
          testParams.push_back(TestParam{
              fileFormat,
              TestMode::kBucketed,
              CommitStrategy::kTaskCommit,
              HiveBucketProperty::Kind::kPrestoNative,
              false,
              multiDrivers,
              CompressionKind_ZSTD,
              scaleWriter}
                                   .value);
          testParams.push_back(TestParam{
              fileFormat,
              TestMode::kOnlyBucketed,
              CommitStrategy::kNoCommit,
              HiveBucketProperty::Kind::kHiveCompatible,
              false,
              multiDrivers,
              CompressionKind_ZSTD,
              scaleWriter}
                                   .value);
          testParams.push_back(TestParam{
              fileFormat,
              TestMode::kOnlyBucketed,
              CommitStrategy::kTaskCommit,
              HiveBucketProperty::Kind::kHiveCompatible,
              false,
              multiDrivers,
              CompressionKind_ZSTD,
              scaleWriter}
                                   .value);
          testParams.push_back(TestParam{
              fileFormat,
              TestMode::kOnlyBucketed,
              CommitStrategy::kNoCommit,
              HiveBucketProperty::Kind::kPrestoNative,
              false,
              multiDrivers,
              CompressionKind_ZSTD,
              scaleWriter}
                                   .value);
          testParams.push_back(TestParam{
              fileFormat,
              TestMode::kOnlyBucketed,
              CommitStrategy::kTaskCommit,
              HiveBucketProperty::Kind::kPrestoNative,
              false,
              multiDrivers,
              CompressionKind_ZSTD,
              scaleWriter}
                                   .value);
        }
      }
    }
    return testParams;
  }
};

// Runs a pipeline with read + filter + project (with substr) + write.
TEST_P(AllTableWriterTest, scanFilterProjectWrite) {
  auto filePaths = makeFilePaths(5);
  auto vectors = makeVectors(filePaths.size(), 500);
  for (int i = 0; i < filePaths.size(); i++) {
    writeToFile(filePaths[i]->getPath(), vectors[i]);
  }

  createDuckDbTable(vectors);

  auto outputDirectory = TempDirectoryPath::create();

  auto planBuilder = PlanBuilder();
  auto project = planBuilder.tableScan(rowType_).filter("c2 <> 0").project(
      {"c0", "c1", "c3", "c5", "c2 + c3", "substr(c5, 1, 1)"});

  auto intputTypes = project.planNode()->outputType()->children();
  std::vector<std::string> tableColumnNames = {
      "c0", "c1", "c3", "c5", "c2_plus_c3", "substr_c5"};
  const auto outputType =
      ROW(std::move(tableColumnNames), std::move(intputTypes));

  auto plan = createInsertPlan(
      project,
      outputType,
      outputDirectory->getPath(),
      partitionedBy_,
      bucketProperty_,
      compressionKind_,
      getNumWriters(),
      connector::hive::LocationHandle::TableType::kNew,
      commitStrategy_);

  assertQueryWithWriterConfigs(
      plan, filePaths, "SELECT count(*) FROM tmp WHERE c2 <> 0");

  // To test the correctness of the generated output,
  // We create a new plan that only read that file and then
  // compare that against a duckDB query that runs the whole query.
  if (partitionedBy_.size() > 0) {
    auto newOutputType = getNonPartitionsColumns(partitionedBy_, outputType);
    assertQuery(
        PlanBuilder().tableScan(newOutputType).planNode(),
        makeHiveConnectorSplits(outputDirectory),
        "SELECT c3, c5, c2 + c3, substr(c5, 1, 1) FROM tmp WHERE c2 <> 0");
    verifyTableWriterOutput(outputDirectory->getPath(), newOutputType, false);
  } else {
    assertQuery(
        PlanBuilder().tableScan(outputType).planNode(),
        makeHiveConnectorSplits(outputDirectory),
        "SELECT c0, c1, c3, c5, c2 + c3, substr(c5, 1, 1) FROM tmp WHERE c2 <> 0");
    verifyTableWriterOutput(outputDirectory->getPath(), outputType, false);
  }
}

TEST_P(AllTableWriterTest, renameAndReorderColumns) {
  auto filePaths = makeFilePaths(5);
  auto vectors = makeVectors(filePaths.size(), 500);
  for (int i = 0; i < filePaths.size(); ++i) {
    writeToFile(filePaths[i]->getPath(), vectors[i]);
  }

  createDuckDbTable(vectors);

  auto outputDirectory = TempDirectoryPath::create();

  if (testMode_ == TestMode::kPartitioned || testMode_ == TestMode::kBucketed) {
    const std::vector<std::string> partitionBy = {"x", "y"};
    setPartitionBy(partitionBy);
  }
  if (testMode_ == TestMode::kBucketed ||
      testMode_ == TestMode::kOnlyBucketed) {
    setBucketProperty(
        bucketProperty_->kind(),
        bucketProperty_->bucketCount(),
        {"z", "v"},
        {REAL(), VARCHAR()},
        {});
  }

  auto inputRowType =
      ROW({"c2", "c5", "c4", "c1", "c0", "c3"},
          {SMALLINT(), VARCHAR(), DOUBLE(), INTEGER(), BIGINT(), REAL()});

  setTableSchema(
      ROW({"u", "v", "w", "x", "y", "z"},
          {SMALLINT(), VARCHAR(), DOUBLE(), INTEGER(), BIGINT(), REAL()}));

  auto plan = createInsertPlan(
      PlanBuilder().tableScan(rowType_),
      inputRowType,
      tableSchema_,
      outputDirectory->getPath(),
      partitionedBy_,
      bucketProperty_,
      compressionKind_,
      getNumWriters(),
      connector::hive::LocationHandle::TableType::kNew,
      commitStrategy_);

  assertQueryWithWriterConfigs(plan, filePaths, "SELECT count(*) FROM tmp");

  if (partitionedBy_.size() > 0) {
    auto newOutputType = getNonPartitionsColumns(partitionedBy_, tableSchema_);
    HiveConnectorTestBase::assertQuery(
        PlanBuilder().tableScan(newOutputType).planNode(),
        makeHiveConnectorSplits(outputDirectory),
        "SELECT c2, c5, c4, c3 FROM tmp");

    verifyTableWriterOutput(outputDirectory->getPath(), newOutputType, false);
  } else {
    HiveConnectorTestBase::assertQuery(
        PlanBuilder().tableScan(tableSchema_).planNode(),
        makeHiveConnectorSplits(outputDirectory),
        "SELECT c2, c5, c4, c1, c0, c3 FROM tmp");

    verifyTableWriterOutput(outputDirectory->getPath(), tableSchema_, false);
  }
}

// Runs a pipeline with read + write.
TEST_P(AllTableWriterTest, directReadWrite) {
  auto filePaths = makeFilePaths(5);
  auto vectors = makeVectors(filePaths.size(), 200);
  for (int i = 0; i < filePaths.size(); i++) {
    writeToFile(filePaths[i]->getPath(), vectors[i]);
  }

  createDuckDbTable(vectors);

  auto outputDirectory = TempDirectoryPath::create();
  auto plan = createInsertPlan(
      PlanBuilder().tableScan(rowType_),
      rowType_,
      outputDirectory->getPath(),
      partitionedBy_,
      bucketProperty_,
      compressionKind_,
      getNumWriters(),
      connector::hive::LocationHandle::TableType::kNew,
      commitStrategy_);

  assertQuery(plan, filePaths, "SELECT count(*) FROM tmp");

  // To test the correctness of the generated output,
  // We create a new plan that only read that file and then
  // compare that against a duckDB query that runs the whole query.

  if (partitionedBy_.size() > 0) {
    auto newOutputType = getNonPartitionsColumns(partitionedBy_, tableSchema_);
    assertQuery(
        PlanBuilder().tableScan(newOutputType).planNode(),
        makeHiveConnectorSplits(outputDirectory),
        "SELECT c2, c3, c4, c5 FROM tmp");
    rowType_ = newOutputType;
    verifyTableWriterOutput(outputDirectory->getPath(), rowType_);
  } else {
    assertQuery(
        PlanBuilder().tableScan(rowType_).planNode(),
        makeHiveConnectorSplits(outputDirectory),
        "SELECT * FROM tmp");

    verifyTableWriterOutput(outputDirectory->getPath(), rowType_);
  }
}

// Tests writing constant vectors.
TEST_P(AllTableWriterTest, constantVectors) {
  vector_size_t size = 1'000;

  // Make constant vectors of various types with null and non-null values.
  auto vector = makeConstantVector(size);

  createDuckDbTable({vector});

  auto outputDirectory = TempDirectoryPath::create();
  auto op = createInsertPlan(
      PlanBuilder().values({vector}),
      rowType_,
      outputDirectory->getPath(),
      partitionedBy_,
      bucketProperty_,
      compressionKind_,
      getNumWriters(),
      connector::hive::LocationHandle::TableType::kNew,
      commitStrategy_);

  assertQuery(op, fmt::format("SELECT {}", size));

  if (partitionedBy_.size() > 0) {
    auto newOutputType = getNonPartitionsColumns(partitionedBy_, tableSchema_);
    assertQuery(
        PlanBuilder().tableScan(newOutputType).planNode(),
        makeHiveConnectorSplits(outputDirectory),
        "SELECT c2, c3, c4, c5 FROM tmp");
    rowType_ = newOutputType;
    verifyTableWriterOutput(outputDirectory->getPath(), rowType_);
  } else {
    assertQuery(
        PlanBuilder().tableScan(rowType_).planNode(),
        makeHiveConnectorSplits(outputDirectory),
        "SELECT * FROM tmp");

    verifyTableWriterOutput(outputDirectory->getPath(), rowType_);
  }
}

TEST_P(AllTableWriterTest, emptyInput) {
  auto outputDirectory = TempDirectoryPath::create();
  auto vector = makeConstantVector(0);
  auto op = createInsertPlan(
      PlanBuilder().values({vector}),
      rowType_,
      outputDirectory->getPath(),
      partitionedBy_,
      bucketProperty_,
      compressionKind_,
      getNumWriters(),
      connector::hive::LocationHandle::TableType::kNew,
      commitStrategy_);

  assertQuery(op, "SELECT 0");
}

TEST_P(AllTableWriterTest, commitStrategies) {
  auto filePaths = makeFilePaths(5);
  auto vectors = makeVectors(filePaths.size(), 100);

  createDuckDbTable(vectors);

  // Test the kTaskCommit commit strategy writing to one dot-prefixed
  // temporary file.
  {
    SCOPED_TRACE(CommitStrategy::kTaskCommit);
    auto outputDirectory = TempDirectoryPath::create();
    auto plan = createInsertPlan(
        PlanBuilder().values(vectors),
        rowType_,
        outputDirectory->getPath(),
        partitionedBy_,
        bucketProperty_,
        compressionKind_,
        getNumWriters(),
        connector::hive::LocationHandle::TableType::kNew,
        commitStrategy_);

    assertQuery(plan, "SELECT count(*) FROM tmp");

    if (partitionedBy_.size() > 0) {
      auto newOutputType =
          getNonPartitionsColumns(partitionedBy_, tableSchema_);
      assertQuery(
          PlanBuilder().tableScan(newOutputType).planNode(),
          makeHiveConnectorSplits(outputDirectory),
          "SELECT c2, c3, c4, c5 FROM tmp");
      auto originalRowType = rowType_;
      rowType_ = newOutputType;
      verifyTableWriterOutput(outputDirectory->getPath(), rowType_);
      rowType_ = originalRowType;
    } else {
      assertQuery(
          PlanBuilder().tableScan(rowType_).planNode(),
          makeHiveConnectorSplits(outputDirectory),
          "SELECT * FROM tmp");
      verifyTableWriterOutput(outputDirectory->getPath(), rowType_);
    }
  }
  // Test kNoCommit commit strategy writing to non-temporary files.
  {
    SCOPED_TRACE(CommitStrategy::kNoCommit);
    auto outputDirectory = TempDirectoryPath::create();
    setCommitStrategy(CommitStrategy::kNoCommit);
    auto plan = createInsertPlan(
        PlanBuilder().values(vectors),
        rowType_,
        outputDirectory->getPath(),
        partitionedBy_,
        bucketProperty_,
        compressionKind_,
        getNumWriters(),
        connector::hive::LocationHandle::TableType::kNew,
        commitStrategy_);

    assertQuery(plan, "SELECT count(*) FROM tmp");

    if (partitionedBy_.size() > 0) {
      auto newOutputType =
          getNonPartitionsColumns(partitionedBy_, tableSchema_);
      assertQuery(
          PlanBuilder().tableScan(newOutputType).planNode(),
          makeHiveConnectorSplits(outputDirectory),
          "SELECT c2, c3, c4, c5 FROM tmp");
      rowType_ = newOutputType;
      verifyTableWriterOutput(outputDirectory->getPath(), rowType_);
    } else {
      assertQuery(
          PlanBuilder().tableScan(rowType_).planNode(),
          makeHiveConnectorSplits(outputDirectory),
          "SELECT * FROM tmp");
      verifyTableWriterOutput(outputDirectory->getPath(), rowType_);
    }
  }
}

TEST_P(PartitionedTableWriterTest, specialPartitionName) {
  const int32_t numPartitions = 50;
  const int32_t numBatches = 2;

  const auto rowType =
      ROW({"c0", "p0", "p1", "c1", "c3", "c5"},
          {INTEGER(), INTEGER(), VARCHAR(), BIGINT(), REAL(), VARCHAR()});
  const std::vector<std::string> partitionKeys = {"p0", "p1"};
  const std::vector<TypePtr> partitionTypes = {INTEGER(), VARCHAR()};

  const std::vector charsToEscape = {
      '"',
      '#',
      '%',
      '\'',
      '*',
      '/',
      ':',
      '=',
      '?',
      '\\',
      '\x7F',
      '{',
      '[',
      ']',
      '^'};
  ASSERT_GE(numPartitions, charsToEscape.size());
  std::vector<RowVectorPtr> vectors = makeBatches(numBatches, [&](auto) {
    return makeRowVector(
        rowType->names(),
        {
            makeFlatVector<int32_t>(
                numPartitions, [&](auto row) { return row + 100; }),
            makeFlatVector<int32_t>(
                numPartitions, [&](auto row) { return row; }),
            makeFlatVector<StringView>(
                numPartitions,
                [&](auto row) {
                  // special character
                  return StringView::makeInline(
                      fmt::format("str_{}{}", row, charsToEscape.at(row % 15)));
                }),
            makeFlatVector<int64_t>(
                numPartitions, [&](auto row) { return row + 1000; }),
            makeFlatVector<float>(
                numPartitions, [&](auto row) { return row + 33.23; }),
            makeFlatVector<StringView>(
                numPartitions,
                [&](auto row) {
                  return StringView::makeInline(
                      fmt::format("bucket_{}", row * 3));
                }),
        });
  });
  createDuckDbTable(vectors);

  auto inputFilePaths = makeFilePaths(numBatches);
  for (int i = 0; i < numBatches; i++) {
    writeToFile(inputFilePaths[i]->getPath(), vectors[i]);
  }

  auto outputDirectory = TempDirectoryPath::create();
  auto plan = createInsertPlan(
      PlanBuilder().tableScan(rowType),
      rowType,
      outputDirectory->getPath(),
      partitionKeys,
      bucketProperty_,
      compressionKind_,
      getNumWriters(),
      connector::hive::LocationHandle::TableType::kNew,
      commitStrategy_);

  auto task = assertQuery(plan, inputFilePaths, "SELECT count(*) FROM tmp");

  std::set<std::string> actualPartitionDirectories =
      getLeafSubdirectories(outputDirectory->getPath());

  std::set<std::string> expectedPartitionDirectories;
  const std::vector<std::string> expectedCharsAfterEscape = {
      "%22",
      "%23",
      "%25",
      "%27",
      "%2A",
      "%2F",
      "%3A",
      "%3D",
      "%3F",
      "%5C",
      "%7F",
      "%7B",
      "%5B",
      "%5D",
      "%5E"};
  for (auto i = 0; i < numPartitions; ++i) {
    // url encoded
    auto partitionName = fmt::format(
        "p0={}/p1=str_{}{}", i, i, expectedCharsAfterEscape.at(i % 15));
    expectedPartitionDirectories.emplace(
        fs::path(outputDirectory->getPath()) / partitionName);
  }
  EXPECT_EQ(actualPartitionDirectories, expectedPartitionDirectories);
}

TEST_P(PartitionedTableWriterTest, multiplePartitions) {
  int32_t numPartitions = 50;
  int32_t numBatches = 2;

  auto rowType =
      ROW({"c0", "p0", "p1", "c1", "c3", "c5"},
          {INTEGER(), INTEGER(), VARCHAR(), BIGINT(), REAL(), VARCHAR()});
  std::vector<std::string> partitionKeys = {"p0", "p1"};
  std::vector<TypePtr> partitionTypes = {INTEGER(), VARCHAR()};

  std::vector<RowVectorPtr> vectors = makeBatches(numBatches, [&](auto) {
    return makeRowVector(
        rowType->names(),
        {
            makeFlatVector<int32_t>(
                numPartitions, [&](auto row) { return row + 100; }),
            makeFlatVector<int32_t>(
                numPartitions, [&](auto row) { return row; }),
            makeFlatVector<StringView>(
                numPartitions,
                [&](auto row) {
                  return StringView::makeInline(fmt::format("str_{}", row));
                }),
            makeFlatVector<int64_t>(
                numPartitions, [&](auto row) { return row + 1000; }),
            makeFlatVector<float>(
                numPartitions, [&](auto row) { return row + 33.23; }),
            makeFlatVector<StringView>(
                numPartitions,
                [&](auto row) {
                  return StringView::makeInline(
                      fmt::format("bucket_{}", row * 3));
                }),
        });
  });
  createDuckDbTable(vectors);

  auto inputFilePaths = makeFilePaths(numBatches);
  for (int i = 0; i < numBatches; i++) {
    writeToFile(inputFilePaths[i]->getPath(), vectors[i]);
  }

  auto outputDirectory = TempDirectoryPath::create();
  auto plan = createInsertPlan(
      PlanBuilder().tableScan(rowType),
      rowType,
      outputDirectory->getPath(),
      partitionKeys,
      bucketProperty_,
      compressionKind_,
      getNumWriters(),
      connector::hive::LocationHandle::TableType::kNew,
      commitStrategy_);

  auto task = assertQuery(plan, inputFilePaths, "SELECT count(*) FROM tmp");

  // Verify that there is one partition directory for each partition.
  std::set<std::string> actualPartitionDirectories =
      getLeafSubdirectories(outputDirectory->getPath());

  std::set<std::string> expectedPartitionDirectories;
  std::set<std::string> partitionNames;
  for (auto i = 0; i < numPartitions; i++) {
    auto partitionName = fmt::format("p0={}/p1=str_{}", i, i);
    partitionNames.emplace(partitionName);
    expectedPartitionDirectories.emplace(
        fs::path(outputDirectory->getPath()) / partitionName);
  }
  EXPECT_EQ(actualPartitionDirectories, expectedPartitionDirectories);

  // Verify distribution of records in partition directories.
  auto iterPartitionDirectory = actualPartitionDirectories.begin();
  auto iterPartitionName = partitionNames.begin();
  auto newOutputType = getNonPartitionsColumns(partitionKeys, rowType);
  while (iterPartitionDirectory != actualPartitionDirectories.end()) {
    assertQuery(
        PlanBuilder().tableScan(newOutputType).planNode(),
        makeHiveConnectorSplits(*iterPartitionDirectory),
        fmt::format(
            "SELECT c0, c1, c3, c5 FROM tmp WHERE {}",
            partitionNameToPredicate(*iterPartitionName, partitionTypes)));
    // In case of unbucketed partitioned table, one single file is written to
    // each partition directory for Hive connector.
    if (testMode_ == TestMode::kPartitioned) {
      ASSERT_EQ(countRecursiveFiles(*iterPartitionDirectory), 1);
    } else {
      ASSERT_GE(countRecursiveFiles(*iterPartitionDirectory), 1);
    }

    ++iterPartitionDirectory;
    ++iterPartitionName;
  }
}

TEST_P(PartitionedTableWriterTest, singlePartition) {
  const int32_t numBatches = 2;
  auto rowType =
      ROW({"c0", "p0", "c3", "c5"}, {VARCHAR(), BIGINT(), REAL(), VARCHAR()});
  std::vector<std::string> partitionKeys = {"p0"};

  // Partition vector is constant vector.
  std::vector<RowVectorPtr> vectors = makeBatches(numBatches, [&](auto) {
    return makeRowVector(
        rowType->names(),
        {makeFlatVector<StringView>(
             1'000,
             [&](auto row) {
               return StringView::makeInline(fmt::format("str_{}", row));
             }),
         makeConstant((int64_t)365, 1'000),
         makeFlatVector<float>(1'000, [&](auto row) { return row + 33.23; }),
         makeFlatVector<StringView>(1'000, [&](auto row) {
           return StringView::makeInline(fmt::format("bucket_{}", row * 3));
         })});
  });
  createDuckDbTable(vectors);

  auto inputFilePaths = makeFilePaths(numBatches);
  for (int i = 0; i < numBatches; i++) {
    writeToFile(inputFilePaths[i]->getPath(), vectors[i]);
  }

  auto outputDirectory = TempDirectoryPath::create();
  const int numWriters = getNumWriters();
  auto plan = createInsertPlan(
      PlanBuilder().tableScan(rowType),
      rowType,
      outputDirectory->getPath(),
      partitionKeys,
      bucketProperty_,
      compressionKind_,
      numWriters,
      connector::hive::LocationHandle::TableType::kNew,
      commitStrategy_);

  auto task = assertQueryWithWriterConfigs(
      plan, inputFilePaths, "SELECT count(*) FROM tmp");

  std::set<std::string> partitionDirectories =
      getLeafSubdirectories(outputDirectory->getPath());

  // Verify only a single partition directory is created.
  ASSERT_EQ(partitionDirectories.size(), 1);
  EXPECT_EQ(
      *partitionDirectories.begin(),
      fs::path(outputDirectory->getPath()) / "p0=365");

  // Verify all data is written to the single partition directory.
  auto newOutputType = getNonPartitionsColumns(partitionKeys, rowType);
  assertQuery(
      PlanBuilder().tableScan(newOutputType).planNode(),
      makeHiveConnectorSplits(outputDirectory),
      "SELECT c0, c3, c5 FROM tmp");

  // In case of unbucketed partitioned table, one single file is written to
  // each partition directory for Hive connector.
  if (testMode_ == TestMode::kPartitioned) {
    ASSERT_LE(countRecursiveFiles(*partitionDirectories.begin()), numWriters);
  } else {
    ASSERT_GE(countRecursiveFiles(*partitionDirectories.begin()), numWriters);
  }
}

TEST_P(PartitionedWithoutBucketTableWriterTest, fromSinglePartitionToMultiple) {
  auto rowType = ROW({"c0", "c1"}, {BIGINT(), BIGINT()});
  setDataTypes(rowType);
  std::vector<std::string> partitionKeys = {"c0"};

  // Partition vector is constant vector.
  std::vector<RowVectorPtr> vectors;
  // The initial vector has the same partition key value;
  vectors.push_back(makeRowVector(
      rowType->names(),
      {makeFlatVector<int64_t>(1'000, [&](auto /*unused*/) { return 1; }),
       makeFlatVector<int64_t>(1'000, [&](auto row) { return row + 1; })}));
  // The second vector has different partition key value.
  vectors.push_back(makeRowVector(
      rowType->names(),
      {makeFlatVector<int64_t>(1'000, [&](auto row) { return row * 234 % 30; }),
       makeFlatVector<int64_t>(1'000, [&](auto row) { return row + 1; })}));
  createDuckDbTable(vectors);

  auto outputDirectory = TempDirectoryPath::create();
  auto plan = createInsertPlan(
      PlanBuilder().values(vectors),
      rowType,
      outputDirectory->getPath(),
      partitionKeys,
      nullptr,
      compressionKind_,
      numTableWriterCount_);

  assertQueryWithWriterConfigs(plan, "SELECT count(*) FROM tmp");

  auto newOutputType = getNonPartitionsColumns(partitionKeys, rowType);
  assertQuery(
      PlanBuilder().tableScan(newOutputType).planNode(),
      makeHiveConnectorSplits(outputDirectory),
      "SELECT c1 FROM tmp");
}

TEST_P(PartitionedTableWriterTest, maxPartitions) {
  SCOPED_TRACE(testParam_.toString());
  const int32_t maxPartitions = 100;
  const int32_t numPartitions =
      testMode_ == TestMode::kBucketed ? 1 : maxPartitions + 1;
  if (testMode_ == TestMode::kBucketed) {
    setBucketProperty(
        testParam_.bucketKind(),
        1000,
        bucketProperty_->bucketedBy(),
        bucketProperty_->bucketedTypes(),
        bucketProperty_->sortedBy());
  }

  auto rowType = ROW({"p0", "c3", "c5"}, {BIGINT(), REAL(), VARCHAR()});
  std::vector<std::string> partitionKeys = {"p0"};

  RowVectorPtr vector;
  if (testMode_ == TestMode::kPartitioned) {
    vector = makeRowVector(
        rowType->names(),
        {makeFlatVector<int64_t>(numPartitions, [&](auto row) { return row; }),
         makeFlatVector<float>(
             numPartitions, [&](auto row) { return row + 33.23; }),
         makeFlatVector<StringView>(numPartitions, [&](auto row) {
           return StringView::makeInline(fmt::format("bucket_{}", row * 3));
         })});
  } else {
    vector = makeRowVector(
        rowType->names(),
        {makeFlatVector<int64_t>(4'000, [&](auto /*unused*/) { return 0; }),
         makeFlatVector<float>(4'000, [&](auto row) { return row + 33.23; }),
         makeFlatVector<StringView>(4'000, [&](auto row) {
           return StringView::makeInline(fmt::format("bucket_{}", row * 3));
         })});
  };

  auto outputDirectory = TempDirectoryPath::create();
  auto plan = createInsertPlan(
      PlanBuilder().values({vector}),
      rowType,
      outputDirectory->getPath(),
      partitionKeys,
      bucketProperty_,
      compressionKind_,
      getNumWriters(),
      connector::hive::LocationHandle::TableType::kNew,
      commitStrategy_);

  if (testMode_ == TestMode::kPartitioned) {
    VELOX_ASSERT_THROW(
        AssertQueryBuilder(plan)
            .connectorSessionProperty(
                kHiveConnectorId,
                HiveConfig::kMaxPartitionsPerWritersSession,
                folly::to<std::string>(maxPartitions))
            .copyResults(pool()),
        fmt::format(
            "Exceeded limit of {} distinct partitions.", maxPartitions));
  } else {
    VELOX_ASSERT_THROW(
        AssertQueryBuilder(plan)
            .connectorSessionProperty(
                kHiveConnectorId,
                HiveConfig::kMaxPartitionsPerWritersSession,
                folly::to<std::string>(maxPartitions))
            .copyResults(pool()),
        "Exceeded open writer limit");
  }
}

// Test TableWriter does not create a file if input is empty.
TEST_P(AllTableWriterTest, writeNoFile) {
  auto outputDirectory = TempDirectoryPath::create();
  auto plan = createInsertPlan(
      PlanBuilder().tableScan(rowType_).filter("false"),
      rowType_,
      outputDirectory->getPath());

  auto execute = [&](const std::shared_ptr<const core::PlanNode>& plan,
                     std::shared_ptr<core::QueryCtx> queryCtx) {
    CursorParameters params;
    params.planNode = plan;
    params.queryCtx = queryCtx;
    readCursor(params, [&](TaskCursor* taskCursor) {
      if (taskCursor->noMoreSplits()) {
        return;
      }
      taskCursor->task()->noMoreSplits("0");
      taskCursor->setNoMoreSplits();
    });
  };

  execute(plan, core::QueryCtx::create(executor_.get()));
  ASSERT_TRUE(fs::is_empty(outputDirectory->getPath()));
}

TEST_P(UnpartitionedTableWriterTest, differentCompression) {
  std::vector<CompressionKind> compressions{
      CompressionKind_NONE,
      CompressionKind_ZLIB,
      CompressionKind_SNAPPY,
      CompressionKind_LZO,
      CompressionKind_ZSTD,
      CompressionKind_LZ4,
      CompressionKind_GZIP,
      CompressionKind_MAX};

  for (auto compressionKind : compressions) {
    auto input = makeVectors(10, 10);
    auto outputDirectory = TempDirectoryPath::create();
    if (compressionKind == CompressionKind_MAX) {
      VELOX_ASSERT_THROW(
          createInsertPlan(
              PlanBuilder().values(input),
              rowType_,
              outputDirectory->getPath(),
              {},
              nullptr,
              compressionKind,
              numTableWriterCount_,
              connector::hive::LocationHandle::TableType::kNew),
          "Unsupported compression type: CompressionKind_MAX");
      return;
    }
    auto plan = createInsertPlan(
        PlanBuilder().values(input),
        rowType_,
        outputDirectory->getPath(),
        {},
        nullptr,
        compressionKind,
        numTableWriterCount_,
        connector::hive::LocationHandle::TableType::kNew);

    // currently we don't support any compression in PARQUET format
    if (fileFormat_ == FileFormat::PARQUET &&
        compressionKind != CompressionKind_NONE) {
      continue;
    }
    if (compressionKind == CompressionKind_NONE ||
        compressionKind == CompressionKind_ZLIB ||
        compressionKind == CompressionKind_ZSTD) {
      auto result = AssertQueryBuilder(plan)
                        .config(
                            QueryConfig::kTaskWriterCount,
                            std::to_string(numTableWriterCount_))
                        .copyResults(pool());
      assertEqualResults(
          {makeRowVector({makeConstant<int64_t>(100, 1)})}, {result});
    } else {
      VELOX_ASSERT_THROW(
          AssertQueryBuilder(plan)
              .config(
                  QueryConfig::kTaskWriterCount,
                  std::to_string(numTableWriterCount_))
              .copyResults(pool()),
          "Unsupported compression type:");
    }
  }
}

TEST_P(UnpartitionedTableWriterTest, runtimeStatsCheck) {
  // The runtime stats test only applies for dwrf file format.
  if (fileFormat_ != dwio::common::FileFormat::DWRF) {
    return;
  }
  struct {
    int numInputVectors;
    std::string maxStripeSize;
    int expectedNumStripes;

    std::string debugString() const {
      return fmt::format(
          "numInputVectors: {}, maxStripeSize: {}, expectedNumStripes: {}",
          numInputVectors,
          maxStripeSize,
          expectedNumStripes);
    }
  } testSettings[] = {
      {10, "1GB", 1},
      {1, "1GB", 1},
      {2, "1GB", 1},
      {10, "1B", 10},
      {2, "1B", 2},
      {1, "1B", 1}};

  for (const auto& testData : testSettings) {
    SCOPED_TRACE(testData.debugString());
    auto rowType = ROW({"c0", "c1"}, {VARCHAR(), BIGINT()});

    VectorFuzzer::Options options;
    options.nullRatio = 0.0;
    options.vectorSize = 1;
    options.stringLength = 1L << 20;
    VectorFuzzer fuzzer(options, pool());

    std::vector<RowVectorPtr> vectors;
    for (int i = 0; i < testData.numInputVectors; ++i) {
      vectors.push_back(fuzzer.fuzzInputRow(rowType));
    }

    createDuckDbTable(vectors);

    auto outputDirectory = TempDirectoryPath::create();
    auto plan = createInsertPlan(
        PlanBuilder().values(vectors),
        rowType,
        outputDirectory->getPath(),
        {},
        nullptr,
        compressionKind_,
        1,
        connector::hive::LocationHandle::TableType::kNew);
    const std::shared_ptr<Task> task =
        AssertQueryBuilder(plan, duckDbQueryRunner_)
            .config(QueryConfig::kTaskWriterCount, std::to_string(1))
            .connectorSessionProperty(
                kHiveConnectorId,
                dwrf::Config::kOrcWriterMaxStripeSizeSession,
                testData.maxStripeSize)
            .assertResults("SELECT count(*) FROM tmp");
    auto stats = task->taskStats().pipelineStats.front().operatorStats;
    if (testData.maxStripeSize == "1GB") {
      ASSERT_GT(
          stats[1].memoryStats.peakTotalMemoryReservation,
          testData.numInputVectors * options.stringLength);
    }
    ASSERT_EQ(
        stats[1].runtimeStats["stripeSize"].count, testData.expectedNumStripes);
    ASSERT_EQ(stats[1].runtimeStats[TableWriter::kNumWrittenFiles].sum, 1);
    ASSERT_EQ(stats[1].runtimeStats[TableWriter::kNumWrittenFiles].count, 1);
    ASSERT_GE(stats[1].runtimeStats[TableWriter::kWriteIOTime].sum, 0);
    ASSERT_EQ(stats[1].runtimeStats[TableWriter::kWriteIOTime].count, 1);
  }
}

TEST_P(UnpartitionedTableWriterTest, immutableSettings) {
  struct {
    connector::hive::LocationHandle::TableType dataType;
    bool immutablePartitionsEnabled;
    bool expectedInsertSuccees;

    std::string debugString() const {
      return fmt::format(
          "dataType:{}, immutablePartitionsEnabled:{}, operationSuccess:{}",
          dataType,
          immutablePartitionsEnabled,
          expectedInsertSuccees);
    }
  } testSettings[] = {
      {connector::hive::LocationHandle::TableType::kNew, true, true},
      {connector::hive::LocationHandle::TableType::kNew, false, true},
      {connector::hive::LocationHandle::TableType::kExisting, true, false},
      {connector::hive::LocationHandle::TableType::kExisting, false, true}};

  for (auto testData : testSettings) {
    SCOPED_TRACE(testData.debugString());
    std::unordered_map<std::string, std::string> propFromFile{
        {"hive.immutable-partitions",
         testData.immutablePartitionsEnabled ? "true" : "false"}};
    std::shared_ptr<const config::ConfigBase> config{
        std::make_shared<config::ConfigBase>(std::move(propFromFile))};
    resetHiveConnector(config);

    auto input = makeVectors(10, 10);
    auto outputDirectory = TempDirectoryPath::create();
    auto plan = createInsertPlan(
        PlanBuilder().values(input),
        rowType_,
        outputDirectory->getPath(),
        {},
        nullptr,
        CompressionKind_NONE,
        numTableWriterCount_,
        testData.dataType);

    if (!testData.expectedInsertSuccees) {
      VELOX_ASSERT_THROW(
          AssertQueryBuilder(plan).copyResults(pool()),
          "Unpartitioned Hive tables are immutable.");
    } else {
      auto result = AssertQueryBuilder(plan)
                        .config(
                            QueryConfig::kTaskWriterCount,
                            std::to_string(numTableWriterCount_))
                        .copyResults(pool());
      assertEqualResults(
          {makeRowVector({makeConstant<int64_t>(100, 1)})}, {result});
    }
  }
}

TEST_P(BucketedUnpartitionedTableWriterTest, bucketNonPartitioned) {
  SCOPED_TRACE(testParam_.toString());
  auto input = makeVectors(1, 100);
  createDuckDbTable(input);

  auto outputDirectory = TempDirectoryPath::create();
  setBucketProperty(
      bucketProperty_->kind(),
      bucketProperty_->bucketCount(),
      bucketProperty_->bucketedBy(),
      bucketProperty_->bucketedTypes(),
      bucketProperty_->sortedBy());
  auto plan = createInsertPlan(
      PlanBuilder().values({input}),
      rowType_,
      outputDirectory->getPath(),
      {},
      bucketProperty_,
      compressionKind_,
      getNumWriters(),
      connector::hive::LocationHandle::TableType::kExisting,
      commitStrategy_);
  assertQueryWithWriterConfigs(plan, "SELECT count(*) FROM tmp");

  assertQuery(
      PlanBuilder().tableScan(rowType_).planNode(),
      makeHiveConnectorSplits(outputDirectory),
      "SELECT * FROM tmp");
  verifyTableWriterOutput(outputDirectory->getPath(), rowType_);
}

TEST_P(BucketedTableOnlyWriteTest, bucketCountLimit) {
  SCOPED_TRACE(testParam_.toString());
  auto input = makeVectors(1, 100);
  createDuckDbTable(input);

  // Get the HiveConfig to access the configurable maxBucketCount
  auto defaultHiveConfig =
      std::make_shared<const HiveConfig>(std::make_shared<config::ConfigBase>(
          std::unordered_map<std::string, std::string>()));

  auto emptySession = std::make_shared<config::ConfigBase>(
      std::unordered_map<std::string, std::string>());
  uint32_t maxBucketCount =
      defaultHiveConfig->maxBucketCount(emptySession.get());

  struct {
    uint32_t bucketCount;
    bool expectedError;

    std::string debugString() const {
      return fmt::format(
          "bucketCount:{} expectedError:{}", bucketCount, expectedError);
    }
  } testSettings[] = {
      {1, false},
      {3, false},
      {maxBucketCount - 1, false},
      {maxBucketCount, true},
      {maxBucketCount + 1, true},
      {maxBucketCount * 2, true}};
  for (const auto& testData : testSettings) {
    SCOPED_TRACE(testData.debugString());
    auto outputDirectory = TempDirectoryPath::create();
    setBucketProperty(
        bucketProperty_->kind(),
        testData.bucketCount,
        bucketProperty_->bucketedBy(),
        bucketProperty_->bucketedTypes(),
        bucketProperty_->sortedBy());
    auto plan = createInsertPlan(
        PlanBuilder().values({input}),
        rowType_,
        outputDirectory->getPath(),
        partitionedBy_,
        bucketProperty_,
        compressionKind_,
        getNumWriters(),
        connector::hive::LocationHandle::TableType::kNew,
        commitStrategy_);
    if (testData.expectedError) {
      VELOX_ASSERT_THROW(
          AssertQueryBuilder(plan)
              .connectorSessionProperty(
                  kHiveConnectorId,
                  HiveConfig::kMaxPartitionsPerWritersSession,
                  // Make sure we have a sufficient large writer limit.
                  folly::to<std::string>(testData.bucketCount * 2))
              .copyResults(pool()),
          "bucketCount exceeds the limit");
    } else {
      assertQueryWithWriterConfigs(plan, "SELECT count(*) FROM tmp");

      if (partitionedBy_.size() > 0) {
        auto newOutputType =
            getNonPartitionsColumns(partitionedBy_, tableSchema_);
        assertQuery(
            PlanBuilder().tableScan(newOutputType).planNode(),
            makeHiveConnectorSplits(outputDirectory),
            "SELECT c2, c3, c4, c5 FROM tmp");
        auto originalRowType = rowType_;
        rowType_ = newOutputType;
        verifyTableWriterOutput(outputDirectory->getPath(), rowType_);
        rowType_ = originalRowType;
      } else {
        assertQuery(
            PlanBuilder().tableScan(rowType_).planNode(),
            makeHiveConnectorSplits(outputDirectory),
            "SELECT * FROM tmp");
        verifyTableWriterOutput(outputDirectory->getPath(), rowType_);
      }
    }
  }
}

TEST_P(BucketedTableOnlyWriteTest, mismatchedBucketTypes) {
  SCOPED_TRACE(testParam_.toString());
  auto input = makeVectors(1, 100);
  createDuckDbTable(input);
  auto outputDirectory = TempDirectoryPath::create();
  std::vector<TypePtr> badBucketedBy = bucketProperty_->bucketedTypes();
  const auto oldType = badBucketedBy[0];
  badBucketedBy[0] = VARCHAR();
  setBucketProperty(
      bucketProperty_->kind(),
      bucketProperty_->bucketCount(),
      bucketProperty_->bucketedBy(),
      badBucketedBy,
      bucketProperty_->sortedBy());
  auto plan = createInsertPlan(
      PlanBuilder().values({input}),
      rowType_,
      outputDirectory->getPath(),
      partitionedBy_,
      bucketProperty_,
      compressionKind_,
      getNumWriters(),
      connector::hive::LocationHandle::TableType::kNew,
      commitStrategy_);
  VELOX_ASSERT_THROW(
      AssertQueryBuilder(plan).copyResults(pool()),
      fmt::format(
          "Input column {} type {} doesn't match bucket type {}",
          bucketProperty_->bucketedBy()[0],
          oldType->toString(),
          bucketProperty_->bucketedTypes()[0]));
}

TEST_P(AllTableWriterTest, tableWriteOutputCheck) {
  SCOPED_TRACE(testParam_.toString());
  if (!testParam_.multiDrivers() ||
      testParam_.testMode() != TestMode::kUnpartitioned) {
    return;
  }
  auto input = makeVectors(10, 100);
  createDuckDbTable(input);
  auto outputDirectory = TempDirectoryPath::create();
  auto plan = createInsertPlan(
      PlanBuilder().values({input}),
      rowType_,
      outputDirectory->getPath(),
      partitionedBy_,
      bucketProperty_,
      compressionKind_,
      getNumWriters(),
      connector::hive::LocationHandle::TableType::kNew,
      commitStrategy_,
      false);

  auto result = runQueryWithWriterConfigs(plan);
  auto writtenRowVector = result->childAt(TableWriteTraits::kRowCountChannel)
                              ->asFlatVector<int64_t>();
  auto fragmentVector = result->childAt(TableWriteTraits::kFragmentChannel)
                            ->asFlatVector<StringView>();
  auto commitContextVector = result->childAt(TableWriteTraits::kContextChannel)
                                 ->asFlatVector<StringView>();
  const int64_t expectedRows = 10 * 100;
  std::vector<std::string> writeFiles;
  int64_t numRows{0};
  for (int i = 0; i < result->size(); ++i) {
    if (testParam_.multiDrivers()) {
      ASSERT_FALSE(commitContextVector->isNullAt(i));
      if (!fragmentVector->isNullAt(i)) {
        ASSERT_TRUE(writtenRowVector->isNullAt(i));
      }
    } else {
      if (i == 0) {
        ASSERT_TRUE(fragmentVector->isNullAt(i));
      } else {
        ASSERT_TRUE(writtenRowVector->isNullAt(i));
        ASSERT_FALSE(fragmentVector->isNullAt(i));
      }
      ASSERT_FALSE(commitContextVector->isNullAt(i));
    }
    if (!fragmentVector->isNullAt(i)) {
      ASSERT_FALSE(fragmentVector->isNullAt(i));
      folly::dynamic obj = folly::parseJson(fragmentVector->valueAt(i));
      if (testMode_ == TestMode::kUnpartitioned) {
        ASSERT_EQ(obj["targetPath"], outputDirectory->getPath());
        ASSERT_EQ(obj["writePath"], outputDirectory->getPath());
      } else {
        std::string partitionDirRe;
        for (const auto& partitionBy : partitionedBy_) {
          partitionDirRe += fmt::format("/{}=.+", partitionBy);
        }
        ASSERT_TRUE(RE2::FullMatch(
            obj["targetPath"].asString(),
            fmt::format("{}{}", outputDirectory->getPath(), partitionDirRe)))
            << obj["targetPath"].asString();
        ASSERT_TRUE(RE2::FullMatch(
            obj["writePath"].asString(),
            fmt::format("{}{}", outputDirectory->getPath(), partitionDirRe)))
            << obj["writePath"].asString();
      }
      numRows += obj["rowCount"].asInt();
      ASSERT_EQ(obj["updateMode"].asString(), "NEW");

      ASSERT_TRUE(obj["fileWriteInfos"].isArray());
      ASSERT_EQ(obj["fileWriteInfos"].size(), 1);
      folly::dynamic writerInfoObj = obj["fileWriteInfos"][0];
      const std::string writeFileName =
          writerInfoObj["writeFileName"].asString();
      writeFiles.push_back(writeFileName);
      const std::string targetFileName =
          writerInfoObj["targetFileName"].asString();
      const std::string writeFileFullPath =
          obj["writePath"].asString() + "/" + writeFileName;
      std::filesystem::path path{writeFileFullPath};
      const auto actualFileSize = fs::file_size(path);
      ASSERT_EQ(obj["onDiskDataSizeInBytes"].asInt(), actualFileSize);
      ASSERT_GT(obj["inMemoryDataSizeInBytes"].asInt(), 0);
      ASSERT_EQ(writerInfoObj["fileSize"], actualFileSize);
      if (commitStrategy_ == CommitStrategy::kNoCommit) {
        ASSERT_EQ(writeFileName, targetFileName);
      } else {
        const std::string kParquetSuffix = ".parquet";
        if (folly::StringPiece(targetFileName).endsWith(kParquetSuffix)) {
          // Remove the .parquet suffix.
          auto trimmedFilename = targetFileName.substr(
              0, targetFileName.size() - kParquetSuffix.size());
          ASSERT_TRUE(writeFileName.find(trimmedFilename) != std::string::npos);
        } else {
          ASSERT_TRUE(writeFileName.find(targetFileName) != std::string::npos);
        }
      }
    }
    if (!commitContextVector->isNullAt(i)) {
      ASSERT_TRUE(RE2::FullMatch(
          commitContextVector->valueAt(i).getString(),
          fmt::format(".*{}.*", commitStrategyToString(commitStrategy_))))
          << commitContextVector->valueAt(i);
    }
  }
  ASSERT_EQ(numRows, expectedRows);
  if (testMode_ == TestMode::kUnpartitioned) {
    ASSERT_GT(writeFiles.size(), 0);
    ASSERT_LE(writeFiles.size(), numTableWriterCount_);
  }
  auto diskFiles = listAllFiles(outputDirectory->getPath());
  std::sort(diskFiles.begin(), diskFiles.end());
  std::sort(writeFiles.begin(), writeFiles.end());
  ASSERT_EQ(diskFiles, writeFiles)
      << "\nwrite files: " << folly::join(",", writeFiles)
      << "\ndisk files: " << folly::join(",", diskFiles);
  // Verify the utilities provided by table writer traits.
  ASSERT_EQ(TableWriteTraits::getRowCount(result), 10 * 100);
  auto obj = TableWriteTraits::getTableCommitContext(result);
  ASSERT_EQ(
      obj[TableWriteTraits::kCommitStrategyContextKey],
      commitStrategyToString(commitStrategy_));
  ASSERT_EQ(obj[TableWriteTraits::klastPageContextKey], true);
  ASSERT_EQ(obj[TableWriteTraits::kLifeSpanContextKey], "TaskWide");
}

TEST_P(AllTableWriterTest, columnStatsDataTypes) {
  auto rowType =
      ROW({"c0", "c1", "c2", "c3", "c4", "c5", "c6", "c7", "c8"},
          {BIGINT(),
           INTEGER(),
           SMALLINT(),
           REAL(),
           DOUBLE(),
           VARCHAR(),
           BOOLEAN(),
           MAP(DATE(), BIGINT()),
           ARRAY(BIGINT())});
  setDataTypes(rowType);
  std::vector<RowVectorPtr> input;
  input.push_back(makeRowVector(
      rowType_->names(),
      {
          makeFlatVector<int64_t>(1'000, [&](auto row) { return 1; }),
          makeFlatVector<int32_t>(1'000, [&](auto row) { return 1; }),
          makeFlatVector<int16_t>(1'000, [&](auto row) { return row; }),
          makeFlatVector<float>(1'000, [&](auto row) { return row + 33.23; }),
          makeFlatVector<double>(1'000, [&](auto row) { return row + 33.23; }),
          makeFlatVector<StringView>(
              1'000,
              [&](auto row) {
                return StringView(std::to_string(row).c_str());
              }),
          makeFlatVector<bool>(1'000, [&](auto row) { return true; }),
          makeMapVector<int32_t, int64_t>(
              1'000,
              [](auto /*row*/) { return 5; },
              [](auto row) { return row; },
              [](auto row) { return row * 3; }),
          makeArrayVector<int64_t>(
              1'000,
              [](auto /*row*/) { return 5; },
              [](auto row) { return row * 3; }),
      }));
  createDuckDbTable(input);
  auto outputDirectory = TempDirectoryPath::create();

  std::vector<FieldAccessTypedExprPtr> groupingKeyFields;
  for (int i = 0; i < partitionedBy_.size(); ++i) {
    groupingKeyFields.emplace_back(std::make_shared<core::FieldAccessTypedExpr>(
        partitionTypes_.at(i), partitionedBy_.at(i)));
  }

  // aggregation node
  core::TypedExprPtr intInputField =
      std::make_shared<const core::FieldAccessTypedExpr>(SMALLINT(), "c2");
  auto minCallExpr = std::make_shared<const core::CallTypedExpr>(
      SMALLINT(), std::vector<core::TypedExprPtr>{intInputField}, "min");
  auto maxCallExpr = std::make_shared<const core::CallTypedExpr>(
      SMALLINT(), std::vector<core::TypedExprPtr>{intInputField}, "max");
  auto distinctCountCallExpr = std::make_shared<const core::CallTypedExpr>(
      VARBINARY(),
      std::vector<core::TypedExprPtr>{intInputField},
      "approx_distinct");

  core::TypedExprPtr strInputField =
      std::make_shared<const core::FieldAccessTypedExpr>(VARCHAR(), "c5");
  auto maxDataSizeCallExpr = std::make_shared<const core::CallTypedExpr>(
      BIGINT(),
      std::vector<core::TypedExprPtr>{strInputField},
      "max_data_size_for_stats");
  auto sumDataSizeCallExpr = std::make_shared<const core::CallTypedExpr>(
      BIGINT(),
      std::vector<core::TypedExprPtr>{strInputField},
      "sum_data_size_for_stats");

  core::TypedExprPtr boolInputField =
      std::make_shared<const core::FieldAccessTypedExpr>(BOOLEAN(), "c6");
  auto countCallExpr = std::make_shared<const core::CallTypedExpr>(
      BIGINT(), std::vector<core::TypedExprPtr>{boolInputField}, "count");
  auto countIfCallExpr = std::make_shared<const core::CallTypedExpr>(
      BIGINT(), std::vector<core::TypedExprPtr>{boolInputField}, "count_if");

  core::TypedExprPtr mapInputField =
      std::make_shared<const core::FieldAccessTypedExpr>(
          MAP(DATE(), BIGINT()), "c7");
  auto countMapCallExpr = std::make_shared<const core::CallTypedExpr>(
      BIGINT(), std::vector<core::TypedExprPtr>{mapInputField}, "count");
  auto sumDataSizeMapCallExpr = std::make_shared<const core::CallTypedExpr>(
      BIGINT(),
      std::vector<core::TypedExprPtr>{mapInputField},
      "sum_data_size_for_stats");

  core::TypedExprPtr arrayInputField =
      std::make_shared<const core::FieldAccessTypedExpr>(
          MAP(DATE(), BIGINT()), "c7");
  auto countArrayCallExpr = std::make_shared<const core::CallTypedExpr>(
      BIGINT(), std::vector<core::TypedExprPtr>{mapInputField}, "count");
  auto sumDataSizeArrayCallExpr = std::make_shared<const core::CallTypedExpr>(
      BIGINT(),
      std::vector<core::TypedExprPtr>{mapInputField},
      "sum_data_size_for_stats");

  const std::vector<std::string> aggregateNames = {
      "min",
      "max",
      "approx_distinct",
      "max_data_size_for_stats",
      "sum_data_size_for_stats",
      "count",
      "count_if",
      "count",
      "sum_data_size_for_stats",
      "count",
      "sum_data_size_for_stats",
  };

  auto makeAggregate = [](const auto& callExpr) {
    std::vector<TypePtr> rawInputTypes;
    for (const auto& input : callExpr->inputs()) {
      rawInputTypes.push_back(input->type());
    }
    return core::AggregationNode::Aggregate{
        callExpr,
        rawInputTypes,
        nullptr, // mask
        {}, // sortingKeys
        {} // sortingOrders
    };
  };

  std::vector<core::AggregationNode::Aggregate> aggregates = {
      makeAggregate(minCallExpr),
      makeAggregate(maxCallExpr),
      makeAggregate(distinctCountCallExpr),
      makeAggregate(maxDataSizeCallExpr),
      makeAggregate(sumDataSizeCallExpr),
      makeAggregate(countCallExpr),
      makeAggregate(countIfCallExpr),
      makeAggregate(countMapCallExpr),
      makeAggregate(sumDataSizeMapCallExpr),
      makeAggregate(countArrayCallExpr),
      makeAggregate(sumDataSizeArrayCallExpr),
  };
  const auto aggregationNode = std::make_shared<core::AggregationNode>(
      core::PlanNodeId(),
      core::AggregationNode::Step::kPartial,
      groupingKeyFields,
      std::vector<core::FieldAccessTypedExprPtr>{},
      aggregateNames,
      aggregates,
      false, // ignoreNullKeys
      PlanBuilder().values({input}).planNode());

  auto plan = PlanBuilder()
                  .values({input})
                  .addNode(addTableWriter(
                      rowType_,
                      rowType_->names(),
                      aggregationNode,
                      std::make_shared<core::InsertTableHandle>(
                          kHiveConnectorId,
                          makeHiveInsertTableHandle(
                              rowType_->names(),
                              rowType_->children(),
                              partitionedBy_,
                              nullptr,
                              makeLocationHandle(outputDirectory->getPath()))),
                      false,
                      CommitStrategy::kNoCommit))
                  .planNode();

  // the result is in format of : row/fragments/context/[partition]/[stats]
  int nextColumnStatsIndex = 3 + partitionedBy_.size();
  const RowVectorPtr result = AssertQueryBuilder(plan).copyResults(pool());
  auto minStatsVector =
      result->childAt(nextColumnStatsIndex++)->asFlatVector<int16_t>();
  ASSERT_EQ(minStatsVector->valueAt(0), 0);
  const auto maxStatsVector =
      result->childAt(nextColumnStatsIndex++)->asFlatVector<int16_t>();
  ASSERT_EQ(maxStatsVector->valueAt(0), 999);
  const auto distinctCountStatsVector =
      result->childAt(nextColumnStatsIndex++)->asFlatVector<StringView>();
  HashStringAllocator allocator{pool_.get()};
  DenseHll denseHll{
      std::string(distinctCountStatsVector->valueAt(0)).c_str(), &allocator};
  ASSERT_EQ(denseHll.cardinality(), 1000);
  const auto maxDataSizeStatsVector =
      result->childAt(nextColumnStatsIndex++)->asFlatVector<int64_t>();
  ASSERT_EQ(maxDataSizeStatsVector->valueAt(0), 7);
  const auto sumDataSizeStatsVector =
      result->childAt(nextColumnStatsIndex++)->asFlatVector<int64_t>();
  ASSERT_EQ(sumDataSizeStatsVector->valueAt(0), 6890);
  const auto countStatsVector =
      result->childAt(nextColumnStatsIndex++)->asFlatVector<int64_t>();
  ASSERT_EQ(countStatsVector->valueAt(0), 1000);
  const auto countIfStatsVector =
      result->childAt(nextColumnStatsIndex++)->asFlatVector<int64_t>();
  ASSERT_EQ(countIfStatsVector->valueAt(0), 1000);
  const auto countMapStatsVector =
      result->childAt(nextColumnStatsIndex++)->asFlatVector<int64_t>();
  ASSERT_EQ(countMapStatsVector->valueAt(0), 1000);
  const auto sumDataSizeMapStatsVector =
      result->childAt(nextColumnStatsIndex++)->asFlatVector<int64_t>();
  ASSERT_EQ(sumDataSizeMapStatsVector->valueAt(0), 64000);
  const auto countArrayStatsVector =
      result->childAt(nextColumnStatsIndex++)->asFlatVector<int64_t>();
  ASSERT_EQ(countArrayStatsVector->valueAt(0), 1000);
  const auto sumDataSizeArrayStatsVector =
      result->childAt(nextColumnStatsIndex++)->asFlatVector<int64_t>();
  ASSERT_EQ(sumDataSizeArrayStatsVector->valueAt(0), 64000);
}

TEST_P(AllTableWriterTest, columnStats) {
  auto input = makeVectors(1, 100);
  createDuckDbTable(input);
  auto outputDirectory = TempDirectoryPath::create();

  // 1. standard columns
  std::vector<std::string> output = {
      "numWrittenRows", "fragment", "tableCommitContext"};
  std::vector<TypePtr> types = {BIGINT(), VARBINARY(), VARBINARY()};
  std::vector<core::FieldAccessTypedExprPtr> groupingKeys;
  // 2. partition columns
  for (int i = 0; i < partitionedBy_.size(); i++) {
    groupingKeys.emplace_back(
        std::make_shared<const core::FieldAccessTypedExpr>(
            partitionTypes_.at(i), partitionedBy_.at(i)));
    output.emplace_back(partitionedBy_.at(i));
    types.emplace_back(partitionTypes_.at(i));
  }
  // 3. stats columns
  output.emplace_back("min");
  types.emplace_back(BIGINT());
  const auto writerOutputType = ROW(std::move(output), std::move(types));

  // aggregation node
  auto aggregationNode = generateAggregationNode(
      "c0",
      groupingKeys,
      core::AggregationNode::Step::kPartial,
      PlanBuilder().values({input}).planNode());

  auto plan = PlanBuilder()
                  .values({input})
                  .addNode(addTableWriter(
                      rowType_,
                      rowType_->names(),
                      aggregationNode,
                      std::make_shared<core::InsertTableHandle>(
                          kHiveConnectorId,
                          makeHiveInsertTableHandle(
                              rowType_->names(),
                              rowType_->children(),
                              partitionedBy_,
                              bucketProperty_,
                              makeLocationHandle(outputDirectory->getPath()))),
                      false,
                      commitStrategy_))
                  .planNode();

  auto result = AssertQueryBuilder(plan).copyResults(pool());
  auto rowVector = result->childAt(0)->asFlatVector<int64_t>();
  auto fragmentVector = result->childAt(1)->asFlatVector<StringView>();
  auto columnStatsVector =
      result->childAt(3 + partitionedBy_.size())->asFlatVector<int64_t>();

  std::vector<std::string> writeFiles;

  // For partitioned, expected result is as follows:
  // Row     Fragment           Context       partition           c1_min_value
  // null    null                x            partition1          0
  // null    null                x            partition2          10
  // null    null                x            partition3          15
  // count   null                x            null                null
  // null    partition1_update   x            null                null
  // null    partition1_update   x            null                null
  // null    partition2_update   x            null                null
  // null    partition2_update   x            null                null
  // null    partition3_update   x            null                null
  //
  // Note that we can have multiple same partition_update, they're for
  // different files, but for stats, we would only have one record for each
  // partition
  //
  // For unpartitioned, expected result is:
  // Row     Fragment           Context       partition           c1_min_value
  // null    null                x                                0
  // count   null                x            null                null
  // null    update              x            null                null

  int countRow = 0;
  while (!columnStatsVector->isNullAt(countRow)) {
    countRow++;
  }
  for (int i = 0; i < result->size(); ++i) {
    if (i < countRow) {
      ASSERT_FALSE(columnStatsVector->isNullAt(i));
      ASSERT_TRUE(rowVector->isNullAt(i));
      ASSERT_TRUE(fragmentVector->isNullAt(i));
    } else if (i == countRow) {
      ASSERT_TRUE(columnStatsVector->isNullAt(i));
      ASSERT_FALSE(rowVector->isNullAt(i));
      ASSERT_TRUE(fragmentVector->isNullAt(i));
    } else {
      ASSERT_TRUE(columnStatsVector->isNullAt(i));
      ASSERT_TRUE(rowVector->isNullAt(i));
      ASSERT_FALSE(fragmentVector->isNullAt(i));
    }
  }
}

TEST_P(AllTableWriterTest, columnStatsWithTableWriteMerge) {
  auto input = makeVectors(1, 100);
  createDuckDbTable(input);
  auto outputDirectory = TempDirectoryPath::create();

  // 1. standard columns
  std::vector<std::string> output = {
      "numWrittenRows", "fragment", "tableCommitContext"};
  std::vector<TypePtr> types = {BIGINT(), VARBINARY(), VARBINARY()};
  std::vector<core::FieldAccessTypedExprPtr> groupingKeys;
  // 2. partition columns
  for (int i = 0; i < partitionedBy_.size(); i++) {
    groupingKeys.emplace_back(
        std::make_shared<const core::FieldAccessTypedExpr>(
            partitionTypes_.at(i), partitionedBy_.at(i)));
    output.emplace_back(partitionedBy_.at(i));
    types.emplace_back(partitionTypes_.at(i));
  }
  // 3. stats columns
  output.emplace_back("min");
  types.emplace_back(BIGINT());
  const auto writerOutputType = ROW(std::move(output), std::move(types));

  // aggregation node
  auto aggregationNode = generateAggregationNode(
      "c0",
      groupingKeys,
      core::AggregationNode::Step::kPartial,
      PlanBuilder().values({input}).planNode());

  auto tableWriterPlan = PlanBuilder().values({input}).addNode(addTableWriter(
      rowType_,
      rowType_->names(),
      aggregationNode,
      std::make_shared<core::InsertTableHandle>(
          kHiveConnectorId,
          makeHiveInsertTableHandle(
              rowType_->names(),
              rowType_->children(),
              partitionedBy_,
              bucketProperty_,
              makeLocationHandle(outputDirectory->getPath()))),
      false,
      commitStrategy_));

  auto mergeAggregationNode = generateAggregationNode(
      "min",
      groupingKeys,
      core::AggregationNode::Step::kIntermediate,
      std::move(tableWriterPlan.planNode()));

  auto finalPlan = tableWriterPlan.capturePlanNodeId(tableWriteNodeId_)
                       .localPartition(std::vector<std::string>{})
                       .tableWriteMerge(std::move(mergeAggregationNode))
                       .planNode();

  auto result = AssertQueryBuilder(finalPlan).copyResults(pool());
  auto rowVector = result->childAt(0)->asFlatVector<int64_t>();
  auto fragmentVector = result->childAt(1)->asFlatVector<StringView>();
  auto columnStatsVector =
      result->childAt(3 + partitionedBy_.size())->asFlatVector<int64_t>();

  std::vector<std::string> writeFiles;

  // For partitioned, expected result is as follows:
  // Row     Fragment           Context       partition           c1_min_value
  // null    null                x            partition1          0
  // null    null                x            partition2          10
  // null    null                x            partition3          15
  // count   null                x            null                null
  // null    partition1_update   x            null                null
  // null    partition1_update   x            null                null
  // null    partition2_update   x            null                null
  // null    partition2_update   x            null                null
  // null    partition3_update   x            null                null
  //
  // Note that we can have multiple same partition_update, they're for
  // different files, but for stats, we would only have one record for each
  // partition
  //
  // For unpartitioned, expected result is:
  // Row     Fragment           Context       partition           c1_min_value
  // null    null                x                                0
  // count   null                x            null                null
  // null    update              x            null                null

  int statsRow = 0;
  while (columnStatsVector->isNullAt(statsRow) && statsRow < result->size()) {
    ++statsRow;
  }
  for (int i = 1; i < result->size(); ++i) {
    if (i < statsRow) {
      ASSERT_TRUE(rowVector->isNullAt(i));
      ASSERT_FALSE(fragmentVector->isNullAt(i));
      ASSERT_TRUE(columnStatsVector->isNullAt(i));
    } else if (i < result->size() - 1) {
      ASSERT_TRUE(rowVector->isNullAt(i));
      ASSERT_TRUE(fragmentVector->isNullAt(i));
      ASSERT_FALSE(columnStatsVector->isNullAt(i));
    } else {
      ASSERT_FALSE(rowVector->isNullAt(i));
      ASSERT_TRUE(fragmentVector->isNullAt(i));
      ASSERT_TRUE(columnStatsVector->isNullAt(i));
    }
  }
}

// TODO: add partitioned table write update mode tests and more failure tests.

TEST_P(AllTableWriterTest, tableWriterStats) {
  const int32_t numBatches = 2;
  auto rowType =
      ROW({"c0", "p0", "c3", "c5"}, {VARCHAR(), BIGINT(), REAL(), VARCHAR()});
  std::vector<std::string> partitionKeys = {"p0"};

  VectorFuzzer::Options options;
  options.vectorSize = 1000;
  VectorFuzzer fuzzer(options, pool());
  // Partition vector is constant vector.
  std::vector<RowVectorPtr> vectors = makeBatches(numBatches, [&](auto) {
    return makeRowVector(
        rowType->names(),
        {fuzzer.fuzzFlat(VARCHAR()),
         fuzzer.fuzzConstant(BIGINT()),
         fuzzer.fuzzFlat(REAL()),
         fuzzer.fuzzFlat(VARCHAR())});
  });
  createDuckDbTable(vectors);

  auto inputFilePaths = makeFilePaths(numBatches);
  for (int i = 0; i < numBatches; i++) {
    writeToFile(inputFilePaths[i]->getPath(), vectors[i]);
  }

  auto outputDirectory = TempDirectoryPath::create();
  const int numWriters = getNumWriters();
  auto plan = createInsertPlan(
      PlanBuilder().tableScan(rowType),
      rowType,
      outputDirectory->getPath(),
      partitionKeys,
      bucketProperty_,
      compressionKind_,
      numWriters,
      connector::hive::LocationHandle::TableType::kNew,
      commitStrategy_);

  auto task = assertQueryWithWriterConfigs(
      plan, inputFilePaths, "SELECT count(*) FROM tmp");

  // Each batch would create a new partition, numWrittenFiles is same as
  // partition num when not bucketed. When bucketed, it's partitionNum *
  // bucketNum, bucket number is 4
  const int numWrittenFiles =
      bucketProperty_ == nullptr ? numBatches : numBatches * 4;
  // The size of bytes (ORC_MAGIC_LEN) written when the DWRF writer
  // initializes a file.
  const int32_t ORC_HEADER_LEN{3};
  const auto fixedWrittenBytes =
      numWrittenFiles * (fileFormat_ == FileFormat::DWRF ? ORC_HEADER_LEN : 0);

  auto planStats = toPlanStats(task->taskStats());
  auto& stats = planStats.at(tableWriteNodeId_);
  ASSERT_GT(stats.physicalWrittenBytes, fixedWrittenBytes);
  ASSERT_GT(
      stats.operatorStats.at("TableWrite")->physicalWrittenBytes,
      fixedWrittenBytes);
  ASSERT_EQ(
      stats.operatorStats.at("TableWrite")
          ->customStats.at(TableWriter::kNumWrittenFiles)
          .sum,
      numWrittenFiles);
  ASSERT_GE(
      stats.operatorStats.at("TableWrite")
          ->customStats.at(TableWriter::kWriteIOTime)
          .sum,
      0);
  ASSERT_GE(
      stats.operatorStats.at("TableWrite")
          ->customStats.at(TableWriter::kRunningWallNanos)
          .sum,
      0);
}

DEBUG_ONLY_TEST_P(
    UnpartitionedTableWriterTest,
    fileWriterFlushErrorOnDriverClose) {
  VectorFuzzer::Options options;
  const int batchSize = 1000;
  options.vectorSize = batchSize;
  VectorFuzzer fuzzer(options, pool());
  const int numBatches = 10;
  std::vector<RowVectorPtr> vectors;
  int numRows{0};
  for (int i = 0; i < numBatches; ++i) {
    numRows += batchSize;
    vectors.push_back(fuzzer.fuzzRow(rowType_));
  }
  std::atomic<int> writeInputs{0};
  std::atomic<bool> triggerWriterOOM{false};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::Driver::runInternal::addInput",
      std::function<void(Operator*)>([&](Operator* op) {
        if (op->operatorType() != "TableWrite") {
          return;
        }
        if (++writeInputs != 3) {
          return;
        }
        op->operatorCtx()->task()->requestAbort();
        triggerWriterOOM = true;
      }));
  SCOPED_TESTVALUE_SET(
      "facebook::velox::memory::MemoryPoolImpl::reserveThreadSafe",
      std::function<void(memory::MemoryPool*)>([&](memory::MemoryPool* pool) {
        const std::string dictPoolRe(".*dictionary");
        const std::string generalPoolRe(".*general");
        const std::string compressionPoolRe(".*compression");
        if (!RE2::FullMatch(pool->name(), dictPoolRe) &&
            !RE2::FullMatch(pool->name(), generalPoolRe) &&
            !RE2::FullMatch(pool->name(), compressionPoolRe)) {
          return;
        }
        if (!triggerWriterOOM) {
          return;
        }
        VELOX_MEM_POOL_CAP_EXCEEDED("Inject write OOM");
      }));

  auto outputDirectory = TempDirectoryPath::create();
  auto op = createInsertPlan(
      PlanBuilder().values(vectors),
      rowType_,
      outputDirectory->getPath(),
      partitionedBy_,
      bucketProperty_,
      compressionKind_,
      getNumWriters(),
      connector::hive::LocationHandle::TableType::kNew,
      commitStrategy_);

  VELOX_ASSERT_THROW(
      assertQuery(op, fmt::format("SELECT {}", numRows)),
      "Aborted for external error");
}

DEBUG_ONLY_TEST_P(UnpartitionedTableWriterTest, dataSinkAbortError) {
  if (fileFormat_ != FileFormat::DWRF) {
    // NOTE: only test on dwrf writer format as we inject write error in dwrf
    // writer.
    return;
  }
  VectorFuzzer::Options options;
  const int batchSize = 100;
  options.vectorSize = batchSize;
  VectorFuzzer fuzzer(options, pool());
  auto vector = fuzzer.fuzzInputRow(rowType_);

  std::atomic<bool> triggerWriterErrorOnce{true};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::dwrf::Writer::write",
      std::function<void(dwrf::Writer*)>([&](dwrf::Writer* /*unused*/) {
        if (!triggerWriterErrorOnce.exchange(false)) {
          return;
        }
        VELOX_FAIL("inject writer error");
      }));

  std::atomic<bool> triggerAbortErrorOnce{true};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::connector::hive::HiveDataSink::closeInternal",
      std::function<void(const HiveDataSink*)>(
          [&](const HiveDataSink* /*unused*/) {
            if (!triggerAbortErrorOnce.exchange(false)) {
              return;
            }
            VELOX_FAIL("inject abort error");
          }));

  auto outputDirectory = TempDirectoryPath::create();
  auto plan = PlanBuilder()
                  .values({vector})
                  .tableWrite(outputDirectory->getPath(), fileFormat_)
                  .planNode();
  VELOX_ASSERT_THROW(
      AssertQueryBuilder(plan).copyResults(pool()), "inject writer error");
  ASSERT_FALSE(triggerWriterErrorOnce);
  ASSERT_FALSE(triggerAbortErrorOnce);
}

TEST_P(BucketSortOnlyTableWriterTest, sortWriterSpill) {
  SCOPED_TRACE(testParam_.toString());

  const auto vectors = makeVectors(5, 500);
  createDuckDbTable(vectors);

  auto outputDirectory = TempDirectoryPath::create();
  auto op = createInsertPlan(
      PlanBuilder().values(vectors),
      rowType_,
      outputDirectory->getPath(),
      partitionedBy_,
      bucketProperty_,
      compressionKind_,
      getNumWriters(),
      connector::hive::LocationHandle::TableType::kNew,
      commitStrategy_);

  const auto spillStats = globalSpillStats();
  auto task =
      assertQueryWithWriterConfigs(op, fmt::format("SELECT {}", 5 * 500), true);
  if (partitionedBy_.size() > 0) {
    rowType_ = getNonPartitionsColumns(partitionedBy_, rowType_);
    verifyTableWriterOutput(outputDirectory->getPath(), rowType_);
  } else {
    verifyTableWriterOutput(outputDirectory->getPath(), rowType_);
  }

  const auto updatedSpillStats = globalSpillStats();
  ASSERT_GT(updatedSpillStats.spilledBytes, spillStats.spilledBytes);
  ASSERT_GT(updatedSpillStats.spilledPartitions, spillStats.spilledPartitions);
  auto taskStats = toPlanStats(task->taskStats());
  auto& stats = taskStats.at(tableWriteNodeId_);
  ASSERT_GT(stats.spilledRows, 0);
  ASSERT_GT(stats.spilledBytes, 0);
  // One spilled partition per each written files.
  const int numWrittenFiles = stats.customStats["numWrittenFiles"].sum;
  ASSERT_GE(stats.spilledPartitions, numWrittenFiles);
  ASSERT_GT(stats.customStats[Operator::kSpillRuns].sum, 0);
  ASSERT_GT(stats.customStats[Operator::kSpillFillTime].sum, 0);
  ASSERT_GT(stats.customStats[Operator::kSpillSortTime].sum, 0);
  ASSERT_GT(stats.customStats[Operator::kSpillExtractVectorTime].sum, 0);
  ASSERT_GT(stats.customStats[Operator::kSpillSerializationTime].sum, 0);
  ASSERT_GT(stats.customStats[Operator::kSpillFlushTime].sum, 0);
  ASSERT_GT(stats.customStats[Operator::kSpillWrites].sum, 0);
  ASSERT_GT(stats.customStats[Operator::kSpillWriteTime].sum, 0);
}

DEBUG_ONLY_TEST_P(BucketSortOnlyTableWriterTest, outputBatchRows) {
  struct {
    uint32_t maxOutputRows;
    std::string maxOutputBytes;
    int expectedOutputCount;

    // TODO: add output size check with spilling enabled
    std::string debugString() const {
      return fmt::format(
          "maxOutputRows: {}, maxOutputBytes: {}, expectedOutputCount: {}",
          maxOutputRows,
          maxOutputBytes,
          expectedOutputCount);
    }
  } testSettings[] = {// we have 4 buckets thus 4 writers.
                      {10000, "1000kB", 4},
                      // when maxOutputRows = 1, 1000 rows triggers 1000 writes
                      {1, "1kB", 1000},
                      // estimatedRowSize is ~62bytes, when maxOutputSize = 62 *
                      // 100, 1000 rows triggers ~10 writes
                      {10000, "6200B", 12}};

  for (const auto& testData : testSettings) {
    SCOPED_TRACE(testData.debugString());
    std::atomic_int outputCount{0};
    SCOPED_TESTVALUE_SET(
        "facebook::velox::dwrf::Writer::write",
        std::function<void(dwrf::Writer*)>(
            [&](dwrf::Writer* /*unused*/) { ++outputCount; }));

    auto rowType =
        ROW({"c0", "p0", "c1", "c3", "c4", "c5"},
            {VARCHAR(), BIGINT(), INTEGER(), REAL(), DOUBLE(), VARCHAR()});
    std::vector<std::string> partitionKeys = {"p0"};

    // Partition vector is constant vector.
    std::vector<RowVectorPtr> vectors = makeBatches(1, [&](auto) {
      return makeRowVector(
          rowType->names(),
          {makeFlatVector<StringView>(
               1'000,
               [&](auto row) {
                 return StringView::makeInline(fmt::format("str_{}", row));
               }),
           makeConstant((int64_t)365, 1'000),
           makeConstant((int32_t)365, 1'000),
           makeFlatVector<float>(1'000, [&](auto row) { return row + 33.23; }),
           makeFlatVector<double>(1'000, [&](auto row) { return row + 33.23; }),
           makeFlatVector<StringView>(1'000, [&](auto row) {
             return StringView::makeInline(fmt::format("bucket_{}", row * 3));
           })});
    });
    createDuckDbTable(vectors);

    auto outputDirectory = TempDirectoryPath::create();
    auto plan = createInsertPlan(
        PlanBuilder().values({vectors}),
        rowType,
        outputDirectory->getPath(),
        partitionKeys,
        bucketProperty_,
        compressionKind_,
        1,
        connector::hive::LocationHandle::TableType::kNew,
        commitStrategy_);
    const std::shared_ptr<Task> task =
        AssertQueryBuilder(plan, duckDbQueryRunner_)
            .config(QueryConfig::kTaskWriterCount, std::to_string(1))
            .connectorSessionProperty(
                kHiveConnectorId,
                HiveConfig::kSortWriterMaxOutputRowsSession,
                folly::to<std::string>(testData.maxOutputRows))
            .connectorSessionProperty(
                kHiveConnectorId,
                HiveConfig::kSortWriterMaxOutputBytesSession,
                folly::to<std::string>(testData.maxOutputBytes))
            .assertResults("SELECT count(*) FROM tmp");
    auto stats = task->taskStats().pipelineStats.front().operatorStats;
    ASSERT_EQ(outputCount, testData.expectedOutputCount);
  }
}

DEBUG_ONLY_TEST_P(BucketSortOnlyTableWriterTest, yield) {
  auto rowType =
      ROW({"c0", "p0", "c1", "c3", "c4", "c5"},
          {VARCHAR(), BIGINT(), INTEGER(), REAL(), DOUBLE(), VARCHAR()});
  std::vector<std::string> partitionKeys = {"p0"};

  // Partition vector is constant vector.
  std::vector<RowVectorPtr> vectors = makeBatches(1, [&](auto) {
    return makeRowVector(
        rowType->names(),
        {makeFlatVector<StringView>(
             1'000,
             [&](auto row) {
               return StringView::makeInline(fmt::format("str_{}", row));
             }),
         makeConstant((int64_t)365, 1'000),
         makeConstant((int32_t)365, 1'000),
         makeFlatVector<float>(1'000, [&](auto row) { return row + 33.23; }),
         makeFlatVector<double>(1'000, [&](auto row) { return row + 33.23; }),
         makeFlatVector<StringView>(1'000, [&](auto row) {
           return StringView::makeInline(fmt::format("bucket_{}", row * 3));
         })});
  });
  createDuckDbTable(vectors);

  struct {
    uint64_t flushTimeSliceLimitMs;
    bool expectedYield;

    std::string debugString() const {
      return fmt::format(
          "flushTimeSliceLimitMs: {}, expectedYield: {}",
          flushTimeSliceLimitMs,
          expectedYield);
    }
  } testSettings[] = {{0, false}, {1, true}, {10'000, false}};

  for (const auto& testData : testSettings) {
    SCOPED_TRACE(testData.debugString());
    std::atomic_bool injectDelayOnce{true};
    SCOPED_TESTVALUE_SET(
        "facebook::velox::dwrf::Writer::write",
        std::function<void(dwrf::Writer*)>([&](dwrf::Writer* /*unused*/) {
          if (!injectDelayOnce.exchange(false)) {
            return;
          }
          std::this_thread::sleep_for(std::chrono::seconds(2));
        }));
    createDuckDbTable(vectors);

    auto outputDirectory = TempDirectoryPath::create();
    auto plan = createInsertPlan(
        PlanBuilder().values({vectors}),
        rowType,
        outputDirectory->getPath(),
        partitionKeys,
        bucketProperty_,
        compressionKind_,
        1,
        connector::hive::LocationHandle::TableType::kNew,
        commitStrategy_);
    const int prevYieldCount = Driver::yieldCount();
    const std::shared_ptr<Task> task =
        AssertQueryBuilder(plan, duckDbQueryRunner_)
            .config(QueryConfig::kTaskWriterCount, std::to_string(1))
            .connectorSessionProperty(
                kHiveConnectorId,
                HiveConfig::kSortWriterFinishTimeSliceLimitMsSession,
                folly::to<std::string>(testData.flushTimeSliceLimitMs))
            .connectorSessionProperty(
                kHiveConnectorId,
                HiveConfig::kSortWriterMaxOutputRowsSession,
                folly::to<std::string>(100))
            .connectorSessionProperty(
                kHiveConnectorId,
                HiveConfig::kSortWriterMaxOutputBytesSession,
                folly::to<std::string>("1KB"))
            .assertResults("SELECT count(*) FROM tmp");
    auto stats = task->taskStats().pipelineStats.front().operatorStats;
    if (testData.expectedYield) {
      ASSERT_GT(Driver::yieldCount(), prevYieldCount);
    } else {
      ASSERT_EQ(Driver::yieldCount(), prevYieldCount);
    }
  }
}

VELOX_INSTANTIATE_TEST_SUITE_P(
    TableWriterTest,
    UnpartitionedTableWriterTest,
    testing::ValuesIn(UnpartitionedTableWriterTest::getTestParams()));

VELOX_INSTANTIATE_TEST_SUITE_P(
    TableWriterTest,
    BucketedUnpartitionedTableWriterTest,
    testing::ValuesIn(BucketedUnpartitionedTableWriterTest::getTestParams()));

VELOX_INSTANTIATE_TEST_SUITE_P(
    TableWriterTest,
    PartitionedTableWriterTest,
    testing::ValuesIn(PartitionedTableWriterTest::getTestParams()));

VELOX_INSTANTIATE_TEST_SUITE_P(
    TableWriterTest,
    BucketedTableOnlyWriteTest,
    testing::ValuesIn(BucketedTableOnlyWriteTest::getTestParams()));

VELOX_INSTANTIATE_TEST_SUITE_P(
    TableWriterTest,
    AllTableWriterTest,
    testing::ValuesIn(AllTableWriterTest::getTestParams()));

VELOX_INSTANTIATE_TEST_SUITE_P(
    TableWriterTest,
    PartitionedWithoutBucketTableWriterTest,
    testing::ValuesIn(
        PartitionedWithoutBucketTableWriterTest::getTestParams()));

VELOX_INSTANTIATE_TEST_SUITE_P(
    TableWriterTest,
    BucketSortOnlyTableWriterTest,
    testing::ValuesIn(BucketSortOnlyTableWriterTest::getTestParams()));

class TableWriterArbitrationTest : public HiveConnectorTestBase {
 protected:
  void SetUp() override {
    HiveConnectorTestBase::SetUp();
    filesystems::registerLocalFileSystem();
    if (!isRegisteredVectorSerde()) {
      this->registerVectorSerde();
    }

    rowType_ = ROW(
        {{"c0", INTEGER()},
         {"c1", INTEGER()},
         {"c2", VARCHAR()},
         {"c3", VARCHAR()}});
    fuzzerOpts_.vectorSize = 1024;
    fuzzerOpts_.nullRatio = 0;
    fuzzerOpts_.stringVariableLength = false;
    fuzzerOpts_.stringLength = 1024;
    fuzzerOpts_.allowLazyVector = false;
  }

  folly::Random::DefaultGenerator rng_;
  RowTypePtr rowType_;
  VectorFuzzer::Options fuzzerOpts_;
};

DEBUG_ONLY_TEST_F(TableWriterArbitrationTest, reclaimFromTableWriter) {
  VectorFuzzer::Options options;
  const int batchSize = 1'000;
  options.vectorSize = batchSize;
  options.stringVariableLength = false;
  options.stringLength = 500;
  VectorFuzzer fuzzer(options, pool());
  const int numBatches = 20;
  std::vector<RowVectorPtr> vectors;
  int numRows{0};
  for (int i = 0; i < numBatches; ++i) {
    numRows += batchSize;
    vectors.push_back(fuzzer.fuzzRow(rowType_));
  }
  createDuckDbTable(vectors);

  for (bool writerSpillEnabled : {false, true}) {
    {
      SCOPED_TRACE(fmt::format("writerSpillEnabled: {}", writerSpillEnabled));
      auto queryPool = memory::memoryManager()->addRootPool(
          "reclaimFromTableWriter", kQueryMemoryCapacity);
      auto* arbitrator = memory::memoryManager()->arbitrator();
      const int numPrevArbitrationFailures = arbitrator->stats().numFailures;
      const int numPrevNonReclaimableAttempts =
          arbitrator->stats().numNonReclaimableAttempts;
      auto queryCtx = core::QueryCtx::create(
          executor_.get(), QueryConfig{{}}, {}, nullptr, std::move(queryPool));
      ASSERT_EQ(queryCtx->pool()->capacity(), kQueryMemoryCapacity);

      std::atomic_int numInputs{0};
      SCOPED_TESTVALUE_SET(
          "facebook::velox::exec::Driver::runInternal::addInput",
          std::function<void(Operator*)>(([&](Operator* op) {
            if (op->operatorType() != "TableWrite") {
              return;
            }
            // We reclaim memory from table writer connector memory pool which
            // connects to the memory pools inside the hive connector.
            ASSERT_FALSE(op->canReclaim());
            if (++numInputs != numBatches) {
              return;
            }

            const auto fakeAllocationSize =
                kQueryMemoryCapacity - op->pool()->parent()->reservedBytes();
            if (writerSpillEnabled) {
              auto* buffer = op->pool()->allocate(fakeAllocationSize);
              op->pool()->free(buffer, fakeAllocationSize);
            } else {
              VELOX_ASSERT_THROW(
                  op->pool()->allocate(fakeAllocationSize),
                  "Exceeded memory pool");
            }
          })));

      auto spillDirectory = TempDirectoryPath::create();
      auto outputDirectory = TempDirectoryPath::create();
      core::PlanNodeId tableWriteNodeId;
      auto writerPlan =
          PlanBuilder()
              .values(vectors)
              .tableWrite(outputDirectory->getPath())
              .capturePlanNodeId(tableWriteNodeId)
              .project({TableWriteTraits::rowCountColumnName()})
              .singleAggregation(
                  {},
                  {fmt::format(
                      "sum({})", TableWriteTraits::rowCountColumnName())})
              .planNode();
      {
        auto task =
            AssertQueryBuilder(duckDbQueryRunner_)
                .queryCtx(queryCtx)
                .maxDrivers(1)
                .spillDirectory(spillDirectory->getPath())
                .config(core::QueryConfig::kSpillEnabled, writerSpillEnabled)
                .config(
                    core::QueryConfig::kWriterSpillEnabled, writerSpillEnabled)
                // Set 0 file writer flush threshold to always trigger flush
                // in test.
                .config(core::QueryConfig::kWriterFlushThresholdBytes, 0)
                .plan(std::move(writerPlan))
                .assertResults(fmt::format("SELECT {}", numRows));
        auto planStats = toPlanStats(task->taskStats());
        auto& tableWriteStats =
            planStats.at(tableWriteNodeId).operatorStats.at("TableWrite");
        if (writerSpillEnabled) {
          ASSERT_GT(
              tableWriteStats->customStats
                  .at(HiveDataSink::kEarlyFlushedRawBytes)
                  .count,
              0);
          ASSERT_GT(
              tableWriteStats->customStats
                  .at(HiveDataSink::kEarlyFlushedRawBytes)
                  .sum,
              0);
          ASSERT_EQ(
              arbitrator->stats().numFailures, numPrevArbitrationFailures);
        } else {
          ASSERT_EQ(
              tableWriteStats->customStats.count(
                  HiveDataSink::kEarlyFlushedRawBytes),
              0);
          ASSERT_EQ(
              arbitrator->stats().numFailures, numPrevArbitrationFailures + 1);
        }
        ASSERT_EQ(
            arbitrator->stats().numNonReclaimableAttempts,
            numPrevNonReclaimableAttempts);
      }
      waitForAllTasksToBeDeleted(3'000'000);
    }
  }
}

DEBUG_ONLY_TEST_F(TableWriterArbitrationTest, reclaimFromSortTableWriter) {
  VectorFuzzer::Options options;
  const int batchSize = 1'000;
  options.vectorSize = batchSize;
  options.stringVariableLength = false;
  options.stringLength = 1'000;
  VectorFuzzer fuzzer(options, pool());
  const int numBatches = 20;
  std::vector<RowVectorPtr> vectors;
  int numRows{0};
  const auto partitionKeyVector = makeFlatVector<int32_t>(
      batchSize, [&](vector_size_t /*unused*/) { return 0; });
  for (int i = 0; i < numBatches; ++i) {
    numRows += batchSize;
    vectors.push_back(fuzzer.fuzzInputRow(rowType_));
    vectors.back()->childAt(0) = partitionKeyVector;
  }
  createDuckDbTable(vectors);

  for (bool writerSpillEnabled : {false, true}) {
    {
      SCOPED_TRACE(fmt::format("writerSpillEnabled: {}", writerSpillEnabled));
      auto queryPool = memory::memoryManager()->addRootPool(
          "reclaimFromSortTableWriter", kQueryMemoryCapacity);
      auto* arbitrator = memory::memoryManager()->arbitrator();
      const int numPrevArbitrationFailures = arbitrator->stats().numFailures;
      const int numPrevNonReclaimableAttempts =
          arbitrator->stats().numNonReclaimableAttempts;
      auto queryCtx = core::QueryCtx::create(
          executor_.get(), QueryConfig{{}}, {}, nullptr, std::move(queryPool));
      ASSERT_EQ(queryCtx->pool()->capacity(), kQueryMemoryCapacity);

      const auto spillStats = common::globalSpillStats();
      std::atomic<int> numInputs{0};
      SCOPED_TESTVALUE_SET(
          "facebook::velox::exec::Driver::runInternal::addInput",
          std::function<void(Operator*)>(([&](Operator* op) {
            if (op->operatorType() != "TableWrite") {
              return;
            }
            // We reclaim memory from table writer connector memory pool which
            // connects to the memory pools inside the hive connector.
            ASSERT_FALSE(op->canReclaim());
            if (++numInputs != numBatches) {
              return;
            }

            const auto fakeAllocationSize =
                kQueryMemoryCapacity - op->pool()->parent()->reservedBytes();
            if (writerSpillEnabled) {
              auto* buffer = op->pool()->allocate(fakeAllocationSize);
              op->pool()->free(buffer, fakeAllocationSize);
            } else {
              VELOX_ASSERT_THROW(
                  op->pool()->allocate(fakeAllocationSize),
                  "Exceeded memory pool");
            }
          })));

      auto spillDirectory = TempDirectoryPath::create();
      auto outputDirectory = TempDirectoryPath::create();
      auto writerPlan =
          PlanBuilder()
              .values(vectors)
              .tableWrite(
                  outputDirectory->getPath(),
                  {"c0"},
                  4,
                  {"c1"},
                  {
                      std::make_shared<HiveSortingColumn>(
                          "c2", core::SortOrder{false, false}),
                  })
              .project({TableWriteTraits::rowCountColumnName()})
              .singleAggregation(
                  {},
                  {fmt::format(
                      "sum({})", TableWriteTraits::rowCountColumnName())})
              .planNode();

      AssertQueryBuilder(duckDbQueryRunner_)
          .queryCtx(queryCtx)
          .maxDrivers(1)
          .spillDirectory(spillDirectory->getPath())
          .config(core::QueryConfig::kSpillEnabled, writerSpillEnabled)
          .config(core::QueryConfig::kWriterSpillEnabled, writerSpillEnabled)
          // Set 0 file writer flush threshold to always trigger flush in
          // test.
          .config(core::QueryConfig::kWriterFlushThresholdBytes, 0)
          .plan(std::move(writerPlan))
          .assertResults(fmt::format("SELECT {}", numRows));

      ASSERT_EQ(
          arbitrator->stats().numFailures,
          numPrevArbitrationFailures + (writerSpillEnabled ? 0 : 1));
      ASSERT_EQ(
          arbitrator->stats().numNonReclaimableAttempts,
          numPrevNonReclaimableAttempts);

      waitForAllTasksToBeDeleted(3'000'000);
      const auto updatedSpillStats = common::globalSpillStats();
      if (writerSpillEnabled) {
        ASSERT_GT(updatedSpillStats.spilledBytes, spillStats.spilledBytes);
        ASSERT_GT(
            updatedSpillStats.spilledPartitions, spillStats.spilledPartitions);
      } else {
        ASSERT_EQ(updatedSpillStats, spillStats);
      }
    }
  }
}

DEBUG_ONLY_TEST_F(TableWriterArbitrationTest, writerFlushThreshold) {
  VectorFuzzer::Options options;
  const int batchSize = 1'000;
  options.vectorSize = batchSize;
  options.stringVariableLength = false;
  options.stringLength = 1'000;
  const int numBatches = 20;
  const int numRows = numBatches * batchSize;
  std::vector<RowVectorPtr> vectors =
      createVectors(numBatches, rowType_, options);
  createDuckDbTable(vectors);

  struct TestParam {
    uint64_t bytesToReserve{0};
    uint64_t writerFlushThreshold{0};
  };
  const std::vector<TestParam> testParams{
      {0, 0}, {0, 1UL << 30}, {64UL << 20, 1UL << 30}};
  for (const auto& testParam : testParams) {
    SCOPED_TRACE(fmt::format(
        "bytesToReserve: {}, writerFlushThreshold: {}",
        succinctBytes(testParam.bytesToReserve),
        succinctBytes(testParam.writerFlushThreshold)));

    auto queryPool = memory::memoryManager()->addRootPool(
        "writerFlushThreshold", kQueryMemoryCapacity);
    auto* arbitrator = memory::memoryManager()->arbitrator();
    const int numPrevArbitrationFailures = arbitrator->stats().numFailures;
    const int numPrevNonReclaimableAttempts =
        arbitrator->stats().numNonReclaimableAttempts;
    auto queryCtx = core::QueryCtx::create(
        executor_.get(), QueryConfig{{}}, {}, nullptr, std::move(queryPool));
    ASSERT_EQ(queryCtx->pool()->capacity(), kQueryMemoryCapacity);

    memory::MemoryPool* compressionPool{nullptr};
    SCOPED_TESTVALUE_SET(
        "facebook::velox::dwrf::Writer::write",
        std::function<void(dwrf::Writer*)>([&](dwrf::Writer* writer) {
          if (testParam.bytesToReserve == 0 || compressionPool != nullptr) {
            return;
          }
          compressionPool = &(writer->getContext().getMemoryPool(
              dwrf::MemoryUsageCategory::OUTPUT_STREAM));
        }));

    std::atomic<int> numInputs{0};
    SCOPED_TESTVALUE_SET(
        "facebook::velox::exec::Driver::runInternal::addInput",
        std::function<void(Operator*)>(([&](Operator* op) {
          if (op->operatorType() != "TableWrite") {
            return;
          }
          if (++numInputs != numBatches) {
            return;
          }

          if (testParam.bytesToReserve > 0) {
            ASSERT_TRUE(compressionPool != nullptr);
            compressionPool->maybeReserve(testParam.bytesToReserve);
          }

          const auto fakeAllocationSize =
              kQueryMemoryCapacity - op->pool()->parent()->usedBytes();
          if (testParam.writerFlushThreshold == 0) {
            auto* buffer = op->pool()->allocate(fakeAllocationSize);
            op->pool()->free(buffer, fakeAllocationSize);
          } else {
            VELOX_ASSERT_THROW(
                op->pool()->allocate(fakeAllocationSize),
                "Exceeded memory pool");
          }
        })));

    auto spillDirectory = TempDirectoryPath::create();
    auto outputDirectory = TempDirectoryPath::create();
    auto writerPlan =
        PlanBuilder()
            .values(vectors)
            .tableWrite(outputDirectory->getPath())
            .project({TableWriteTraits::rowCountColumnName()})
            .singleAggregation(
                {},
                {fmt::format(
                    "sum({})", TableWriteTraits::rowCountColumnName())})
            .planNode();

    AssertQueryBuilder(duckDbQueryRunner_)
        .queryCtx(queryCtx)
        .maxDrivers(1)
        .spillDirectory(spillDirectory->getPath())
        .config(core::QueryConfig::kSpillEnabled, true)
        .config(core::QueryConfig::kWriterSpillEnabled, true)
        .config(
            core::QueryConfig::kWriterFlushThresholdBytes,
            testParam.writerFlushThreshold)
        .plan(std::move(writerPlan))
        .assertResults(fmt::format("SELECT {}", numRows));

    ASSERT_EQ(
        arbitrator->stats().numFailures,
        numPrevArbitrationFailures +
            (testParam.writerFlushThreshold == 0 ? 0 : 1));
    // We don't trigger reclaim on a writer if it doesn't meet the writer
    // flush threshold.
    ASSERT_EQ(
        arbitrator->stats().numNonReclaimableAttempts,
        numPrevNonReclaimableAttempts);
    ASSERT_GE(arbitrator->stats().reclaimedUsedBytes, testParam.bytesToReserve);
    waitForAllTasksToBeDeleted(3'000'000);
    queryCtx.reset();
  }
}

DEBUG_ONLY_TEST_F(
    TableWriterArbitrationTest,
    reclaimFromNonReclaimableTableWriter) {
  VectorFuzzer::Options options;
  const int batchSize = 1'000;
  options.vectorSize = batchSize;
  options.stringVariableLength = false;
  options.stringLength = 1'000;
  VectorFuzzer fuzzer(options, pool());
  const int numBatches = 20;
  std::vector<RowVectorPtr> vectors;
  int numRows{0};
  for (int i = 0; i < numBatches; ++i) {
    numRows += batchSize;
    vectors.push_back(fuzzer.fuzzRow(rowType_));
  }

  createDuckDbTable(vectors);

  auto queryPool = memory::memoryManager()->addRootPool(
      "reclaimFromNonReclaimableTableWriter", kQueryMemoryCapacity);
  auto* arbitrator = memory::memoryManager()->arbitrator();
  const int numPrevArbitrationFailures = arbitrator->stats().numFailures;
  const int numPrevNonReclaimableAttempts =
      arbitrator->stats().numNonReclaimableAttempts;
  auto queryCtx = core::QueryCtx::create(
      executor_.get(), QueryConfig{{}}, {}, nullptr, std::move(queryPool));
  ASSERT_EQ(queryCtx->pool()->capacity(), kQueryMemoryCapacity);

  std::atomic<bool> injectFakeAllocationOnce{true};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::dwrf::Writer::write",
      std::function<void(dwrf::Writer*)>(([&](dwrf::Writer* writer) {
        if (!injectFakeAllocationOnce.exchange(false)) {
          return;
        }
        auto& pool = writer->getContext().getMemoryPool(
            dwrf::MemoryUsageCategory::GENERAL);
        const auto fakeAllocationSize =
            kQueryMemoryCapacity - pool.reservedBytes();
        VELOX_ASSERT_THROW(
            pool.allocate(fakeAllocationSize), "Exceeded memory pool");
      })));

  auto outputDirectory = TempDirectoryPath::create();
  auto writerPlan =
      PlanBuilder()
          .values(vectors)
          .tableWrite(outputDirectory->getPath())
          .project({TableWriteTraits::rowCountColumnName()})
          .singleAggregation(
              {},
              {fmt::format("sum({})", TableWriteTraits::rowCountColumnName())})
          .planNode();

  const auto spillDirectory = TempDirectoryPath::create();
  AssertQueryBuilder(duckDbQueryRunner_)
      .queryCtx(queryCtx)
      .maxDrivers(1)
      .spillDirectory(spillDirectory->getPath())
      .config(core::QueryConfig::kSpillEnabled, true)
      .config(core::QueryConfig::kWriterSpillEnabled, true)
      // Set file writer flush threshold of zero to always trigger flush in
      // test.
      .config(core::QueryConfig::kWriterFlushThresholdBytes, 0)
      // Set large stripe and dictionary size thresholds to avoid writer
      // internal stripe flush.
      .connectorSessionProperty(
          kHiveConnectorId, dwrf::Config::kOrcWriterMaxStripeSizeSession, "1GB")
      .connectorSessionProperty(
          kHiveConnectorId,
          dwrf::Config::kOrcWriterMaxDictionaryMemorySession,
          "1GB")
      .plan(std::move(writerPlan))
      .assertResults(fmt::format("SELECT {}", numRows));

  ASSERT_EQ(arbitrator->stats().numFailures, numPrevArbitrationFailures + 1);
  ASSERT_EQ(
      arbitrator->stats().numNonReclaimableAttempts,
      numPrevNonReclaimableAttempts + 1);
  waitForAllTasksToBeDeleted();
}

DEBUG_ONLY_TEST_F(
    TableWriterArbitrationTest,
    arbitrationFromTableWriterWithNoMoreInput) {
  VectorFuzzer::Options options;
  const int batchSize = 1'000;
  options.vectorSize = batchSize;
  options.stringVariableLength = false;
  options.stringLength = 1'000;
  VectorFuzzer fuzzer(options, pool());
  const int numBatches = 10;
  std::vector<RowVectorPtr> vectors;
  int numRows{0};
  for (int i = 0; i < numBatches; ++i) {
    numRows += batchSize;
    vectors.push_back(fuzzer.fuzzRow(rowType_));
  }

  createDuckDbTable(vectors);
  auto queryPool = memory::memoryManager()->addRootPool(
      "arbitrationFromTableWriterWithNoMoreInput", kQueryMemoryCapacity);
  auto* arbitrator = memory::memoryManager()->arbitrator();
  const int numPrevArbitrationFailures = arbitrator->stats().numFailures;
  const int numPrevNonReclaimableAttempts =
      arbitrator->stats().numNonReclaimableAttempts;
  const int numPrevReclaimedBytes = arbitrator->stats().reclaimedUsedBytes;
  auto queryCtx = core::QueryCtx::create(
      executor_.get(), QueryConfig{{}}, {}, nullptr, std::move(queryPool));
  ASSERT_EQ(queryCtx->pool()->capacity(), kQueryMemoryCapacity);

  std::atomic<bool> writerNoMoreInput{false};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::Driver::runInternal::noMoreInput",
      std::function<void(Operator*)>(([&](Operator* op) {
        if (op->operatorType() != "TableWrite") {
          return;
        }
        writerNoMoreInput = true;
      })));

  std::atomic<bool> injectGetOutputOnce{true};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::Driver::runInternal::getOutput",
      std::function<void(Operator*)>(([&](Operator* op) {
        if (op->operatorType() != "TableWrite") {
          return;
        }
        if (!writerNoMoreInput) {
          return;
        }
        if (!injectGetOutputOnce.exchange(false)) {
          return;
        }
        const auto fakeAllocationSize =
            kQueryMemoryCapacity - op->pool()->parent()->reservedBytes();
        auto* buffer = op->pool()->allocate(fakeAllocationSize);
        op->pool()->free(buffer, fakeAllocationSize);
      })));

  auto outputDirectory = TempDirectoryPath::create();
  auto writerPlan =
      PlanBuilder()
          .values(vectors)
          .tableWrite(outputDirectory->getPath())
          .project({TableWriteTraits::rowCountColumnName()})
          .singleAggregation(
              {},
              {fmt::format("sum({})", TableWriteTraits::rowCountColumnName())})
          .planNode();

  const auto spillDirectory = TempDirectoryPath::create();
  AssertQueryBuilder(duckDbQueryRunner_)
      .queryCtx(queryCtx)
      .maxDrivers(1)
      .spillDirectory(spillDirectory->getPath())
      .config(core::QueryConfig::kSpillEnabled, true)
      .config(core::QueryConfig::kWriterSpillEnabled, true)
      // Set 0 file writer flush threshold to always trigger flush in test.
      .config(core::QueryConfig::kWriterFlushThresholdBytes, 0)
      // Set large stripe and dictionary size thresholds to avoid writer
      // internal stripe flush.
      .connectorSessionProperty(
          kHiveConnectorId, dwrf::Config::kOrcWriterMaxStripeSizeSession, "1GB")
      .connectorSessionProperty(
          kHiveConnectorId,
          dwrf::Config::kOrcWriterMaxDictionaryMemorySession,
          "1GB")
      .plan(std::move(writerPlan))
      .assertResults(fmt::format("SELECT {}", numRows));

  ASSERT_EQ(
      arbitrator->stats().numNonReclaimableAttempts,
      numPrevArbitrationFailures);
  ASSERT_EQ(arbitrator->stats().numFailures, numPrevNonReclaimableAttempts);
  ASSERT_GT(arbitrator->stats().reclaimedUsedBytes, numPrevReclaimedBytes);
  waitForAllTasksToBeDeleted();
}

DEBUG_ONLY_TEST_F(
    TableWriterArbitrationTest,
    reclaimFromNonReclaimableSortTableWriter) {
  VectorFuzzer::Options options;
  const int batchSize = 1'000;
  options.vectorSize = batchSize;
  options.stringVariableLength = false;
  options.stringLength = 1'000;
  VectorFuzzer fuzzer(options, pool());
  const int numBatches = 20;
  std::vector<RowVectorPtr> vectors;
  int numRows{0};
  const auto partitionKeyVector = makeFlatVector<int32_t>(
      batchSize, [&](vector_size_t /*unused*/) { return 0; });
  for (int i = 0; i < numBatches; ++i) {
    numRows += batchSize;
    vectors.push_back(fuzzer.fuzzInputRow(rowType_));
    vectors.back()->childAt(0) = partitionKeyVector;
  }

  createDuckDbTable(vectors);

  auto queryPool = memory::memoryManager()->addRootPool(
      "reclaimFromNonReclaimableSortTableWriter", kQueryMemoryCapacity);
  auto* arbitrator = memory::memoryManager()->arbitrator();
  const int numPrevArbitrationFailures = arbitrator->stats().numFailures;
  const int numPrevNonReclaimableAttempts =
      arbitrator->stats().numNonReclaimableAttempts;
  auto queryCtx = core::QueryCtx::create(
      executor_.get(), QueryConfig{{}}, {}, nullptr, std::move(queryPool));
  ASSERT_EQ(queryCtx->pool()->capacity(), kQueryMemoryCapacity);

  std::atomic<bool> injectFakeAllocationOnce{true};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::memory::MemoryPoolImpl::reserveThreadSafe",
      std::function<void(memory::MemoryPool*)>(([&](memory::MemoryPool* pool) {
        const std::string re(".*sort");
        if (!RE2::FullMatch(pool->name(), re)) {
          return;
        }
        const int writerMemoryUsage = 4L << 20;
        if (pool->parent()->reservedBytes() < writerMemoryUsage) {
          return;
        }
        if (!injectFakeAllocationOnce.exchange(false)) {
          return;
        }
        const auto fakeAllocationSize =
            kQueryMemoryCapacity - pool->parent()->reservedBytes();
        VELOX_ASSERT_THROW(
            pool->allocate(fakeAllocationSize), "Exceeded memory pool");
      })));

  auto outputDirectory = TempDirectoryPath::create();
  auto writerPlan =
      PlanBuilder()
          .values(vectors)
          .tableWrite(
              outputDirectory->getPath(),
              {"c0"},
              4,
              {"c1"},
              {
                  std::make_shared<HiveSortingColumn>(
                      "c2", core::SortOrder{false, false}),
              })
          .project({TableWriteTraits::rowCountColumnName()})
          .singleAggregation(
              {},
              {fmt::format("sum({})", TableWriteTraits::rowCountColumnName())})
          .planNode();

  const auto spillStats = common::globalSpillStats();
  const auto spillDirectory = TempDirectoryPath::create();
  AssertQueryBuilder(duckDbQueryRunner_)
      .queryCtx(queryCtx)
      .maxDrivers(1)
      .spillDirectory(spillDirectory->getPath())
      .config(core::QueryConfig::kSpillEnabled, "true")
      .config(core::QueryConfig::kWriterSpillEnabled, "true")
      // Set file writer flush threshold of zero to always trigger flush in
      // test.
      .config(core::QueryConfig::kWriterFlushThresholdBytes, "0")
      // Set large stripe and dictionary size thresholds to avoid writer
      // internal stripe flush.
      .connectorSessionProperty(
          kHiveConnectorId, dwrf::Config::kOrcWriterMaxStripeSizeSession, "1GB")
      .connectorSessionProperty(
          kHiveConnectorId,
          dwrf::Config::kOrcWriterMaxDictionaryMemorySession,
          "1GB")
      .plan(std::move(writerPlan))
      .assertResults(fmt::format("SELECT {}", numRows));

  ASSERT_EQ(arbitrator->stats().numFailures, numPrevArbitrationFailures + 1);
  ASSERT_EQ(
      arbitrator->stats().numNonReclaimableAttempts,
      numPrevNonReclaimableAttempts + 1);
  const auto updatedSpillStats = common::globalSpillStats();
  ASSERT_EQ(updatedSpillStats, spillStats);
  waitForAllTasksToBeDeleted();
}

DEBUG_ONLY_TEST_F(TableWriterArbitrationTest, tableFileWriteError) {
  VectorFuzzer::Options options;
  const int batchSize = 1'000;
  options.vectorSize = batchSize;
  options.stringVariableLength = false;
  options.stringLength = 1'000;
  VectorFuzzer fuzzer(options, pool());
  const int numBatches = 20;
  std::vector<RowVectorPtr> vectors;
  for (int i = 0; i < numBatches; ++i) {
    vectors.push_back(fuzzer.fuzzRow(rowType_));
  }

  createDuckDbTable(vectors);

  auto queryPool = memory::memoryManager()->addRootPool(
      "tableFileWriteError", kQueryMemoryCapacity);
  auto queryCtx = core::QueryCtx::create(
      executor_.get(), QueryConfig{{}}, {}, nullptr, std::move(queryPool));
  ASSERT_EQ(queryCtx->pool()->capacity(), kQueryMemoryCapacity);

  std::atomic_bool injectWriterErrorOnce{true};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::dwrf::Writer::write",
      std::function<void(dwrf::Writer*)>(([&](dwrf::Writer* writer) {
        auto& context = writer->getContext();
        auto& pool =
            context.getMemoryPool(dwrf::MemoryUsageCategory::OUTPUT_STREAM);
        if (static_cast<memory::MemoryPoolImpl*>(&pool)
                ->testingMinReservationBytes() == 0) {
          return;
        }
        if (!injectWriterErrorOnce.exchange(false)) {
          return;
        }
        VELOX_FAIL("inject writer error");
      })));

  const auto spillDirectory = TempDirectoryPath::create();
  const auto outputDirectory = TempDirectoryPath::create();
  auto writerPlan = PlanBuilder()
                        .values(vectors)
                        .tableWrite(outputDirectory->getPath())
                        .planNode();
  VELOX_ASSERT_THROW(
      AssertQueryBuilder(duckDbQueryRunner_)
          .queryCtx(queryCtx)
          .maxDrivers(1)
          .spillDirectory(spillDirectory->getPath())
          .config(core::QueryConfig::kSpillEnabled, true)
          .config(core::QueryConfig::kWriterSpillEnabled, true)
          // Set 0 file writer flush threshold to always reclaim memory from
          // file writer.
          .config(core::QueryConfig::kWriterFlushThresholdBytes, 0)
          // Set stripe size to extreme large to avoid writer internal
          // triggered flush.
          .connectorSessionProperty(
              kHiveConnectorId,
              dwrf::Config::kOrcWriterMaxStripeSizeSession,
              "1GB")
          .connectorSessionProperty(
              kHiveConnectorId,
              dwrf::Config::kOrcWriterMaxDictionaryMemorySession,
              "1GB")
          .plan(std::move(writerPlan))
          .copyResults(pool()),
      "inject writer error");

  waitForAllTasksToBeDeleted();
}

DEBUG_ONLY_TEST_F(TableWriterArbitrationTest, tableWriteSpillUseMoreMemory) {
  // Create a large number of vectors to trigger writer spill.
  fuzzerOpts_.vectorSize = 1000;
  fuzzerOpts_.stringLength = 2048;
  fuzzerOpts_.stringVariableLength = false;
  VectorFuzzer fuzzer(fuzzerOpts_, pool());

  std::vector<RowVectorPtr> vectors;
  for (int i = 0; i < 10; ++i) {
    vectors.push_back(fuzzer.fuzzInputRow(rowType_));
  }

  auto queryPool = memory::memoryManager()->addRootPool(
      "tableWriteSpillUseMoreMemory", kQueryMemoryCapacity / 4);
  auto queryCtx = core::QueryCtx::create(
      executor_.get(), QueryConfig{{}}, {}, nullptr, std::move(queryPool));
  ASSERT_EQ(queryCtx->pool()->capacity(), kQueryMemoryCapacity / 4);

  auto fakeLeafPool = queryCtx->pool()->addLeafChild(
      "fakeLeaf", true, FakeMemoryReclaimer::create());
  const int fakeAllocationSize = kQueryMemoryCapacity * 3 / 16;
  TestAllocation injectedFakeAllocation{
      fakeLeafPool.get(),
      fakeLeafPool->allocate(fakeAllocationSize),
      fakeAllocationSize};

  TestAllocation injectedWriterAllocation;
  SCOPED_TESTVALUE_SET(
      "facebook::velox::dwrf::Writer::flushInternal",
      std::function<void(dwrf::Writer*)>(([&](dwrf::Writer* writer) {
        ASSERT_TRUE(memory::underMemoryArbitration());
        injectedFakeAllocation.free();
        auto& pool = writer->getContext().getMemoryPool(
            dwrf::MemoryUsageCategory::GENERAL);
        injectedWriterAllocation.pool = &pool;
        injectedWriterAllocation.size = kQueryMemoryCapacity / 8;
        injectedWriterAllocation.buffer =
            pool.allocate(injectedWriterAllocation.size);
      })));

  // Free the extra fake memory allocations to make memory pool state
  // consistent at the end of test.
  std::atomic_bool clearAllocationOnce{true};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::Task::setError",
      std::function<void(Task*)>(([&](Task* task) {
        if (!clearAllocationOnce.exchange(false)) {
          return;
        }
        ASSERT_EQ(injectedWriterAllocation.size, kQueryMemoryCapacity / 8);
        injectedWriterAllocation.free();
      })));

  const auto spillDirectory = TempDirectoryPath::create();
  const auto outputDirectory = TempDirectoryPath::create();
  auto writerPlan = PlanBuilder()
                        .values(vectors)
                        .tableWrite(outputDirectory->getPath())
                        .planNode();
  VELOX_ASSERT_THROW(
      AssertQueryBuilder(duckDbQueryRunner_)
          .queryCtx(queryCtx)
          .maxDrivers(1)
          .spillDirectory(spillDirectory->getPath())
          .config(core::QueryConfig::kSpillEnabled, true)
          .config(core::QueryConfig::kWriterSpillEnabled, true)
          // Set 0 file writer flush threshold to always trigger flush in
          // test.
          .config(core::QueryConfig::kWriterFlushThresholdBytes, 0)
          // Set stripe size to extreme large to avoid writer internal
          // triggered flush.
          .connectorSessionProperty(
              kHiveConnectorId,
              dwrf::Config::kOrcWriterMaxStripeSizeSession,
              "1GB")
          .connectorSessionProperty(
              kHiveConnectorId,
              dwrf::Config::kOrcWriterMaxDictionaryMemorySession,
              "1GB")
          .plan(std::move(writerPlan))
          .copyResults(pool()),
      "");

  waitForAllTasksToBeDeleted();
}

DEBUG_ONLY_TEST_F(TableWriterArbitrationTest, tableWriteReclaimOnClose) {
  // Create a large number of vectors to trigger writer spill.
  fuzzerOpts_.vectorSize = 1000;
  fuzzerOpts_.stringLength = 1024;
  fuzzerOpts_.stringVariableLength = false;
  VectorFuzzer fuzzer(fuzzerOpts_, pool());
  std::vector<RowVectorPtr> vectors;
  int numRows{0};
  for (int i = 0; i < 10; ++i) {
    vectors.push_back(fuzzer.fuzzInputRow(rowType_));
    numRows += vectors.back()->size();
  }

  auto queryPool = memory::memoryManager()->addRootPool(
      "tableWriteSpillUseMoreMemory", kQueryMemoryCapacity);
  auto queryCtx = core::QueryCtx::create(
      executor_.get(), QueryConfig{{}}, {}, nullptr, std::move(queryPool));
  ASSERT_EQ(queryCtx->pool()->capacity(), kQueryMemoryCapacity);

  auto fakeQueryPool =
      memory::memoryManager()->addRootPool("fake", kQueryMemoryCapacity);
  auto fakeQueryCtx = core::QueryCtx::create(
      executor_.get(), QueryConfig{{}}, {}, nullptr, std::move(fakeQueryPool));
  ASSERT_EQ(fakeQueryCtx->pool()->capacity(), kQueryMemoryCapacity);

  auto fakeLeafPool = fakeQueryCtx->pool()->addLeafChild(
      "fakeLeaf", true, FakeMemoryReclaimer::create());

  std::atomic_bool writerNoMoreInput{false};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::Driver::runInternal::noMoreInput",
      std::function<void(Operator*)>(([&](Operator* op) {
        if (op->operatorType() != "TableWrite") {
          return;
        }
        writerNoMoreInput = true;
      })));

  std::atomic<bool> maybeReserveInjectOnce{true};
  TestAllocation fakeAllocation;
  SCOPED_TESTVALUE_SET(
      "facebook::velox::common::memory::MemoryPoolImpl::maybeReserve",
      std::function<void(memory::MemoryPool*)>([&](memory::MemoryPool* pool) {
        if (!writerNoMoreInput) {
          return;
        }
        if (!maybeReserveInjectOnce.exchange(false)) {
          return;
        }
        // The injection memory allocation to cause maybeReserve on writer
        // close to trigger memory arbitration. The latter tries to reclaim
        // memory from this file writer.
        const size_t injectAllocationSize = kQueryMemoryCapacity;
        fakeAllocation = TestAllocation{
            .pool = fakeLeafPool.get(),
            .buffer = fakeLeafPool->allocate(injectAllocationSize),
            .size = injectAllocationSize};
      }));

  SCOPED_TESTVALUE_SET(
      "facebook::velox::dwrf::Writer::flushStripe",
      std::function<void(dwrf::Writer*)>(
          [&](dwrf::Writer* writer) { fakeAllocation.free(); }));

  const auto spillDirectory = TempDirectoryPath::create();
  const auto outputDirectory = TempDirectoryPath::create();
  auto writerPlan =
      PlanBuilder()
          .values(vectors)
          .tableWrite(outputDirectory->getPath())
          .singleAggregation(
              {},
              {fmt::format("sum({})", TableWriteTraits::rowCountColumnName())})
          .planNode();

  AssertQueryBuilder(duckDbQueryRunner_)
      .queryCtx(queryCtx)
      .maxDrivers(1)
      .spillDirectory(spillDirectory->getPath())
      .config(core::QueryConfig::kSpillEnabled, true)
      .config(core::QueryConfig::kWriterSpillEnabled, true)
      // Set 0 file writer flush threshold to always trigger flush in test.
      .config(core::QueryConfig::kWriterFlushThresholdBytes, 0)
      // Set stripe size to extreme large to avoid writer internal triggered
      // flush.
      .connectorSessionProperty(
          kHiveConnectorId, dwrf::Config::kOrcWriterMaxStripeSizeSession, "1GB")
      .connectorSessionProperty(
          kHiveConnectorId,
          dwrf::Config::kOrcWriterMaxDictionaryMemorySession,
          "1GB")
      .plan(std::move(writerPlan))
      .assertResults(fmt::format("SELECT {}", numRows));

  waitForAllTasksToBeDeleted();
}

DEBUG_ONLY_TEST_F(
    TableWriterArbitrationTest,
    raceBetweenWriterCloseAndTaskReclaim) {
  const uint64_t memoryCapacity = 512 * MB;
  std::vector<RowVectorPtr> vectors =
      createVectors(rowType_, memoryCapacity / 8, fuzzerOpts_);
  const auto expectedResult =
      runWriteTask(vectors, nullptr, false, 1, pool(), kHiveConnectorId, false)
          .data;
  auto queryPool = memory::memoryManager()->addRootPool(
      "tableWriteSpillUseMoreMemory", kQueryMemoryCapacity);
  auto queryCtx = core::QueryCtx::create(
      executor_.get(), QueryConfig{{}}, {}, nullptr, std::move(queryPool));
  ASSERT_EQ(queryCtx->pool()->capacity(), kQueryMemoryCapacity);

  std::atomic_bool writerCloseWaitFlag{true};
  folly::EventCount writerCloseWait;
  std::atomic_bool taskReclaimWaitFlag{true};
  folly::EventCount taskReclaimWait;
  SCOPED_TESTVALUE_SET(
      "facebook::velox::dwrf::Writer::flushStripe",
      std::function<void(dwrf::Writer*)>(([&](dwrf::Writer* writer) {
        writerCloseWaitFlag = false;
        writerCloseWait.notifyAll();
        taskReclaimWait.await([&]() { return !taskReclaimWaitFlag.load(); });
      })));

  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::Task::requestPauseLocked",
      std::function<void(Task*)>(([&](Task* /*unused*/) {
        taskReclaimWaitFlag = false;
        taskReclaimWait.notifyAll();
      })));

  std::thread queryThread([&]() {
    const auto result = runWriteTask(
        vectors,
        queryCtx,
        false,
        1,
        pool(),
        kHiveConnectorId,
        true,
        expectedResult);
  });

  writerCloseWait.await([&]() { return !writerCloseWaitFlag.load(); });

  memory::testingRunArbitration();

  queryThread.join();
  waitForAllTasksToBeDeleted();
}
} // namespace velox::exec::test
