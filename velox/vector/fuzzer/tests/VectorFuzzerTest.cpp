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

#include <boost/random/uniform_int_distribution.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "velox/common/fuzzer/ConstrainedGenerators.h"
#include "velox/common/memory/Memory.h"
#include "velox/functions/prestosql/types/JsonRegistration.h"
#include "velox/functions/prestosql/types/JsonType.h"
#include "velox/functions/prestosql/types/QDigestRegistration.h"
#include "velox/functions/prestosql/types/QDigestType.h"
#include "velox/type/TypeEncodingUtil.h"
#include "velox/vector/DictionaryVector.h"
#include "velox/vector/fuzzer/VectorFuzzer.h"

using namespace facebook::velox;

namespace {

using facebook::velox::fuzzer::JsonInputGenerator;
using facebook::velox::fuzzer::RandomInputGenerator;
using facebook::velox::fuzzer::SetConstrainedGenerator;

class VectorFuzzerTest : public testing::Test {
 public:
  static void SetUpTestCase() {
    memory::MemoryManager::testingSetInstance(memory::MemoryManager::Options{});
    registerJsonType();
    registerQDigestType();
  }

  memory::MemoryPool* pool() const {
    return pool_.get();
  }

  // Asserts that all arrays in the input vector have exactly `containerSize`
  // elements.
  void assertContainerSize(
      VectorPtr vector,
      size_t vectorSize,
      size_t containerSize) {
    auto* arrayMapBase = vector->as<ArrayVectorBase>();
    ASSERT_EQ(vectorSize, vector->size());

    for (size_t i = 0; i < vector->size(); ++i) {
      EXPECT_EQ(containerSize, arrayMapBase->sizeAt(i));
    }
  }

  // Asserts that map keys are unique and not null.
  void assertMapKeys(MapVector* mapVector) {
    auto mapKeys = mapVector->mapKeys();
    ASSERT_FALSE(mapKeys->mayHaveNulls()) << mapKeys->toString();

    std::unordered_map<uint64_t, vector_size_t> map;

    for (size_t i = 0; i < mapVector->size(); ++i) {
      vector_size_t offset = mapVector->offsetAt(i);
      map.clear();

      // For each map element, check that keys are unique. Keep track of the
      // hashes found so far; in case duplicated hashes are found, call the
      // equalAt() function to break hash colisions ties.
      for (size_t j = 0; j < mapVector->sizeAt(i); ++j) {
        vector_size_t idx = offset + j;
        uint64_t hash = mapKeys->hashValueAt(idx);

        auto it = map.find(hash);
        if (it != map.end()) {
          ASSERT_FALSE(mapKeys->equalValueAt(mapKeys.get(), idx, it->second))
              << "Found duplicated map keys: " << mapKeys->toString(idx)
              << " vs. " << mapKeys->toString(it->second);
        } else {
          map.emplace(hash, idx);
        }
      }
    }
  }

  void validateMaxSizes(VectorPtr vector, size_t maxSize);

 private:
  std::shared_ptr<memory::MemoryPool> pool_{
      memory::memoryManager()->addLeafPool()};
};

TEST_F(VectorFuzzerTest, flatPrimitive) {
  VectorFuzzer::Options opts;
  opts.nullRatio = 0.5;
  VectorFuzzer fuzzer(opts, pool());
  VectorPtr vector;

  std::vector<TypePtr> types = {
      TINYINT(),
      BIGINT(),
      DOUBLE(),
      BOOLEAN(),
      VARCHAR(),
      VARBINARY(),
      DATE(),
      TIMESTAMP(),
      INTERVAL_DAY_TIME(),
      UNKNOWN(),
      fuzzer.randShortDecimalType(),
      fuzzer.randLongDecimalType()};

  for (const auto& type : types) {
    vector = fuzzer.fuzzFlat(type);
    ASSERT_EQ(VectorEncoding::Simple::FLAT, vector->encoding());
    ASSERT_TRUE(vector->type()->kindEquals(type));
    ASSERT_EQ(opts.vectorSize, vector->size());
    ASSERT_TRUE(vector->mayHaveNulls());
  }
}

TEST_F(VectorFuzzerTest, flatComplex) {
  VectorFuzzer::Options opts;
  opts.nullRatio = 0.5;
  VectorFuzzer fuzzer(opts, pool());

  // Arrays.
  auto vector = fuzzer.fuzzFlat(ARRAY(BIGINT()));
  ASSERT_EQ(VectorEncoding::Simple::ARRAY, vector->encoding());
  ASSERT_EQ(opts.vectorSize, vector->size());
  ASSERT_TRUE(vector->mayHaveNulls());

  auto elements = vector->as<ArrayVector>()->elements();
  ASSERT_TRUE(elements->type()->kindEquals(BIGINT()));
  ASSERT_EQ(VectorEncoding::Simple::FLAT, elements->encoding());
  ASSERT_EQ(opts.vectorSize * opts.containerLength, elements->size());

  // Maps.
  vector = fuzzer.fuzzFlat(MAP(BIGINT(), DOUBLE()));
  ASSERT_EQ(VectorEncoding::Simple::MAP, vector->encoding());
  ASSERT_EQ(opts.vectorSize, vector->size());
  ASSERT_TRUE(vector->mayHaveNulls());

  auto mapKeys = vector->as<MapVector>()->mapKeys();
  ASSERT_TRUE(mapKeys->type()->kindEquals(BIGINT()));
  ASSERT_EQ(VectorEncoding::Simple::FLAT, mapKeys->encoding());
  ASSERT_EQ(opts.vectorSize * opts.containerLength, mapKeys->size());

  auto mapValues = vector->as<MapVector>()->mapValues();
  ASSERT_TRUE(mapValues->type()->kindEquals(DOUBLE()));
  ASSERT_EQ(VectorEncoding::Simple::FLAT, mapValues->encoding());
  ASSERT_EQ(opts.vectorSize * opts.containerLength, mapValues->size());
}

TEST_F(VectorFuzzerTest, flatNotNull) {
  VectorFuzzer::Options opts;
  opts.nullRatio = 0;
  VectorFuzzer fuzzer(opts, pool());

  auto vector = fuzzer.fuzzFlat(BIGINT());
  ASSERT_FALSE(vector->mayHaveNulls());

  vector = fuzzer.fuzzFlat(ARRAY(BIGINT()));
  ASSERT_FALSE(vector->mayHaveNulls());

  vector = fuzzer.fuzzFlat(MAP(BIGINT(), INTEGER()));
  ASSERT_FALSE(vector->mayHaveNulls());

  // Try the explicit not null API.
  opts.nullRatio = 0.5;
  fuzzer.setOptions(opts);

  vector = fuzzer.fuzzFlat(MAP(BIGINT(), INTEGER()));
  ASSERT_TRUE(vector->mayHaveNulls());

  vector = fuzzer.fuzzFlatNotNull(MAP(BIGINT(), INTEGER()));
  ASSERT_FALSE(vector->mayHaveNulls());
}

struct Foo {
  explicit Foo(int64_t id) : id_(id) {}
  int64_t id_;
};
struct Bar {};

TEST_F(VectorFuzzerTest, flatOpaque) {
  // Exercises fuzzer.flatOpaque()
  VectorFuzzer::Options opts;
  opts.nullRatio = 0.5;
  VectorFuzzer fuzzer(opts, pool());
  fuzzer.registerOpaqueTypeGenerator<Foo>([](FuzzerGenerator& rng) {
    int64_t id = boost::random::uniform_int_distribution<int64_t>(1, 10)(rng);
    return std::make_shared<Foo>(id);
  });
  fuzzer.registerOpaqueTypeGenerator<Bar>([](FuzzerGenerator& rng) {
    throw std::runtime_error("Should not be called");
    return std::make_shared<Bar>();
  });

  auto opaqueType = OPAQUE<Foo>();
  VectorPtr vector = fuzzer.fuzzFlat(opaqueType);
  ASSERT_EQ(VectorEncoding::Simple::FLAT, vector->encoding());
  ASSERT_TRUE(vector->type()->kindEquals(opaqueType));
  ASSERT_EQ(opts.vectorSize, vector->size());
  ASSERT_TRUE(vector->mayHaveNulls());

  auto flatVector = vector->asFlatVector<std::shared_ptr<void>>();
  for (auto i = 0; i < vector->size(); ++i) {
    if (flatVector->isNullAt(i)) {
      continue;
    }
    auto element = std::reinterpret_pointer_cast<Foo>(flatVector->valueAt(i));
    ASSERT_GT(element->id_, 0);
    ASSERT_LT(element->id_, 11);
  }
}

TEST_F(VectorFuzzerTest, opaque) {
  // Exercises fuzzer.fuzz() for opaque types.
  VectorFuzzer::Options opts;
  opts.nullRatio = 0.5;
  VectorFuzzer fuzzer(opts, pool());
  fuzzer.registerOpaqueTypeGenerator<Foo>([](FuzzerGenerator& rng) {
    int64_t id = boost::random::uniform_int_distribution<int64_t>(1, 10)(rng);
    return std::make_shared<Foo>(id);
  });
  fuzzer.registerOpaqueTypeGenerator<Bar>([](FuzzerGenerator& rng) {
    throw std::runtime_error("Should not be called");
    return std::make_shared<Bar>();
  });

  auto opaqueType = OPAQUE<Foo>();
  VectorPtr vector = fuzzer.fuzz(opaqueType, opts.vectorSize);
  // There's a chance of the vector being wrapped in a dictionary or made into a
  // constant.
  ASSERT_TRUE(
      vector->encoding() == VectorEncoding::Simple::DICTIONARY ||
      vector->encoding() == VectorEncoding::Simple::FLAT ||
      vector->encoding() == VectorEncoding::Simple::CONSTANT);
  ASSERT_TRUE(vector->type()->kindEquals(opaqueType));
  ASSERT_EQ(opts.vectorSize, vector->size());
  ASSERT_TRUE(vector->mayHaveNulls());
}

TEST_F(VectorFuzzerTest, dictionary) {
  VectorFuzzer::Options opts;
  VectorFuzzer fuzzer(opts, pool());

  // Generates a flat inner vector without nuls.
  const size_t innerSize = 100;
  auto inner = fuzzer.fuzzFlat(REAL(), innerSize);

  opts.nullRatio = 0.5;
  fuzzer.setOptions(opts);

  // Generate a dictionary with the same size as the inner vector being wrapped.
  auto vector = fuzzer.fuzzDictionary(inner);
  ASSERT_EQ(VectorEncoding::Simple::DICTIONARY, vector->encoding());
  ASSERT_TRUE(vector->mayHaveNulls());
  ASSERT_TRUE(vector->valueVector()->type()->kindEquals(REAL()));
  ASSERT_EQ(innerSize, vector->size());
  ASSERT_EQ(innerSize, vector->valueVector()->size());

  // Generate a dictionary with less elements.
  vector = fuzzer.fuzzDictionary(inner, 10);
  ASSERT_EQ(VectorEncoding::Simple::DICTIONARY, vector->encoding());
  ASSERT_TRUE(vector->mayHaveNulls());
  ASSERT_TRUE(vector->valueVector()->type()->kindEquals(REAL()));
  ASSERT_EQ(10, vector->size());
  ASSERT_EQ(innerSize, vector->valueVector()->size());

  // Generate a dictionary with more elements.
  vector = fuzzer.fuzzDictionary(inner, 1000);
  ASSERT_EQ(VectorEncoding::Simple::DICTIONARY, vector->encoding());
  ASSERT_TRUE(vector->mayHaveNulls());
  ASSERT_TRUE(vector->valueVector()->type()->kindEquals(REAL()));
  ASSERT_EQ(1000, vector->size());
  ASSERT_EQ(innerSize, vector->valueVector()->size());

  // Generate a dictionary without nulls.
  opts.dictionaryHasNulls = false;
  fuzzer.setOptions(opts);
  vector = fuzzer.fuzzDictionary(inner);
  ASSERT_FALSE(vector->mayHaveNulls());
}

TEST_F(VectorFuzzerTest, constants) {
  VectorFuzzer::Options opts;
  opts.nullRatio = 0;
  VectorFuzzer fuzzer(opts, pool());

  // Non-null primitive constants.
  auto vector = fuzzer.fuzzConstant(INTEGER());
  ASSERT_TRUE(vector->type()->kindEquals(INTEGER()));
  ASSERT_EQ(VectorEncoding::Simple::CONSTANT, vector->encoding());
  ASSERT_FALSE(vector->mayHaveNulls());

  vector = fuzzer.fuzzConstant(VARCHAR());
  ASSERT_TRUE(vector->type()->kindEquals(VARCHAR()));
  ASSERT_EQ(VectorEncoding::Simple::CONSTANT, vector->encoding());
  ASSERT_FALSE(vector->mayHaveNulls());

  auto shortDecimalType = fuzzer.randShortDecimalType();
  vector = fuzzer.fuzzConstant(shortDecimalType);
  ASSERT_TRUE(vector->type()->kindEquals(shortDecimalType));
  ASSERT_EQ(VectorEncoding::Simple::CONSTANT, vector->encoding());
  ASSERT_FALSE(vector->mayHaveNulls());

  auto longDecimalType = fuzzer.randLongDecimalType();
  vector = fuzzer.fuzzConstant(longDecimalType);
  ASSERT_TRUE(vector->type()->kindEquals(longDecimalType));
  ASSERT_EQ(VectorEncoding::Simple::CONSTANT, vector->encoding());
  ASSERT_FALSE(vector->mayHaveNulls());

  // Non-null complex types.
  vector = fuzzer.fuzzConstant(MAP(BIGINT(), SMALLINT()));
  ASSERT_TRUE(vector->type()->kindEquals(MAP(BIGINT(), SMALLINT())));
  ASSERT_EQ(VectorEncoding::Simple::CONSTANT, vector->encoding());
  ASSERT_FALSE(vector->mayHaveNulls());

  vector = fuzzer.fuzzConstant(ROW({ARRAY(BIGINT()), SMALLINT()}));
  ASSERT_TRUE(vector->type()->kindEquals(ROW({ARRAY(BIGINT()), SMALLINT()})));
  ASSERT_EQ(VectorEncoding::Simple::CONSTANT, vector->encoding());
  ASSERT_FALSE(vector->mayHaveNulls());

  // Fuzzer should produce null constant for UNKNOWN type even if nullRatio is
  // 0.
  vector = fuzzer.fuzzConstant(UNKNOWN());
  ASSERT_TRUE(vector->type()->kindEquals(UNKNOWN()));
  ASSERT_EQ(VectorEncoding::Simple::CONSTANT, vector->encoding());
  ASSERT_TRUE(vector->mayHaveNulls());
}

TEST_F(VectorFuzzerTest, constantsNull) {
  VectorFuzzer::Options opts;
  opts.nullRatio = 1; // 1 = 100%
  VectorFuzzer fuzzer(opts, pool());

  // Null constants.
  auto vector = fuzzer.fuzzConstant(REAL());
  ASSERT_TRUE(vector->type()->kindEquals(REAL()));
  ASSERT_EQ(VectorEncoding::Simple::CONSTANT, vector->encoding());
  ASSERT_TRUE(vector->mayHaveNulls());

  vector = fuzzer.fuzzConstant(UNKNOWN());
  ASSERT_TRUE(vector->type()->kindEquals(UNKNOWN()));
  ASSERT_EQ(VectorEncoding::Simple::CONSTANT, vector->encoding());
  ASSERT_TRUE(vector->mayHaveNulls());

  // Null complex types.
  vector = fuzzer.fuzzConstant(ARRAY(VARCHAR()));
  ASSERT_TRUE(vector->type()->kindEquals(ARRAY(VARCHAR())));
  ASSERT_EQ(VectorEncoding::Simple::CONSTANT, vector->encoding());
  ASSERT_TRUE(vector->mayHaveNulls());
}

TEST_F(VectorFuzzerTest, array) {
  VectorFuzzer::Options opts;
  opts.containerVariableLength = false;
  VectorFuzzer fuzzer(opts, pool());

  // 1 elements per array.
  auto vector = fuzzer.fuzzArray(fuzzer.fuzzFlat(REAL(), 100), 100);
  ASSERT_TRUE(vector->type()->kindEquals(ARRAY(REAL())));
  assertContainerSize(vector, 100, 1);

  // 10 elements per array.
  vector = fuzzer.fuzzArray(fuzzer.fuzzFlat(REAL(), 100), 10);
  assertContainerSize(vector, 10, 10);

  // 3 elements per array.
  vector = fuzzer.fuzzArray(fuzzer.fuzzFlat(REAL(), 100), 33);
  assertContainerSize(vector, 33, 3);

  // 100 elements per array.
  vector = fuzzer.fuzzArray(fuzzer.fuzzFlat(REAL(), 100), 1);
  assertContainerSize(vector, 1, 100);

  // More array rows than elements.
  vector = fuzzer.fuzzArray(fuzzer.fuzzFlat(REAL(), 100), 1000);

  auto* arrayVector = vector->as<ArrayVector>();
  ASSERT_EQ(vector->size(), 1000);

  // Check that the first 100 arrays have 1 element, and the remaining have 0.
  for (size_t i = 0; i < 100; ++i) {
    EXPECT_EQ(1, arrayVector->sizeAt(i));
  }
  for (size_t i = 100; i < 1000; ++i) {
    EXPECT_EQ(0, arrayVector->sizeAt(i));
  }

  // Variable number of array elements - just ensure we don't exceed the number
  // of underlying elements.
  opts.containerVariableLength = true;
  fuzzer.setOptions(opts);

  size_t arraySize = 100;

  vector = fuzzer.fuzzArray(fuzzer.fuzzFlat(REAL(), 100), arraySize);
  ASSERT_EQ(arraySize, vector->size());
  ASSERT_GE(
      100, vector->offsetAt(arraySize - 1) + vector->sizeAt(arraySize - 1));

  arraySize = 33;

  vector = fuzzer.fuzzArray(fuzzer.fuzzFlat(REAL(), 100), arraySize);
  ASSERT_EQ(arraySize, vector->size());
  ASSERT_GE(
      100, vector->offsetAt(arraySize - 1) + vector->sizeAt(arraySize - 1));

  // Test fuzzArray with given element type. Check that element size doesn't
  // exceed opts.complexElementsMaxSize.
  const auto kElementMaxSize = 100;
  opts.complexElementsMaxSize = kElementMaxSize;
  fuzzer.setOptions(opts);

  auto checkElementSize = [&](size_t arraySize) {
    vector = fuzzer.fuzzArray(BIGINT(), arraySize);
    ASSERT_TRUE(vector->type()->childAt(0)->isBigint());
    ASSERT_EQ(arraySize, vector->size());
    ASSERT_LE(
        vector->offsetAt(arraySize - 1) + vector->sizeAt(arraySize - 1),
        kElementMaxSize);
  };
  checkElementSize(kElementMaxSize);
  checkElementSize(kElementMaxSize - 70);
  checkElementSize(kElementMaxSize + 50);
}

TEST_F(VectorFuzzerTest, map) {
  VectorFuzzer::Options opts;
  opts.containerVariableLength = false;
  VectorFuzzer fuzzer(opts, pool());

  // 1 elements per array.
  auto vector = fuzzer.fuzzMap(
      fuzzer.fuzzFlat(REAL(), 100), fuzzer.fuzzFlat(BIGINT(), 100), 100);
  ASSERT_TRUE(vector->type()->kindEquals(MAP(REAL(), BIGINT())));
  assertContainerSize(vector, 100, 1);

  // 10 elements per array.
  vector = fuzzer.fuzzMap(
      fuzzer.fuzzFlat(REAL(), 100), fuzzer.fuzzFlat(INTEGER(), 100), 10);
  assertContainerSize(vector, 10, 10);
}

// Test that fuzzer return map key which are unique and not null.
TEST_F(VectorFuzzerTest, mapKeys) {
  VectorFuzzer::Options opts;
  opts.nullRatio = 0.5;
  VectorFuzzer fuzzer(opts, pool());

  auto nullableVector = fuzzer.fuzz(BIGINT(), 1000);

  // Check that a nullable key value throws if normalizeMapKeys is true.
  opts.normalizeMapKeys = true;
  fuzzer.setOptions(opts);
  EXPECT_THROW(
      fuzzer.fuzzMap(nullableVector, fuzzer.fuzzFlat(BIGINT(), 1000), 10),
      VeloxRuntimeError);

  opts.normalizeMapKeys = false;
  fuzzer.setOptions(opts);
  EXPECT_NO_THROW(
      fuzzer.fuzzMap(nullableVector, fuzzer.fuzzFlat(BIGINT(), 1000), 10));
  opts.normalizeMapKeys = true;
  fuzzer.setOptions(opts);

  std::vector<TypePtr> types = {
      TINYINT(), SMALLINT(), BIGINT(), DOUBLE(), VARCHAR()};

  for (const auto& keyType : types) {
    for (size_t i = 0; i < 10; ++i) {
      auto vector = fuzzer.fuzzMap(
          fuzzer.fuzzNotNull(keyType, 1000),
          fuzzer.fuzzFlat(BIGINT(), 1000),
          10);
      assertMapKeys(vector->as<MapVector>());
    }
  }
}

TEST_F(VectorFuzzerTest, row) {
  VectorFuzzer::Options opts;
  opts.nullRatio = 0.5;
  VectorFuzzer fuzzer(opts, pool());

  auto vector = fuzzer.fuzzRow(ROW({INTEGER(), REAL(), ARRAY(SMALLINT())}));
  ASSERT_TRUE(
      vector->type()->kindEquals(ROW({INTEGER(), REAL(), ARRAY(SMALLINT())})));
  ASSERT_TRUE(vector->mayHaveNulls());

  // fuzzInputRow() doesn't have top-level nulls.
  vector = fuzzer.fuzzInputRow(ROW({INTEGER(), REAL()}));
  ASSERT_TRUE(vector->type()->kindEquals(ROW({INTEGER(), REAL()})));
  ASSERT_FALSE(vector->mayHaveNulls());

  // Composable API.
  vector = fuzzer.fuzzRow(
      {fuzzer.fuzzFlat(REAL(), 100), fuzzer.fuzzFlat(BIGINT(), 100)},
      {"c0", "c1"},
      100);
  ASSERT_TRUE(vector->type()->kindEquals(ROW({REAL(), BIGINT()})));
  ASSERT_TRUE(vector->mayHaveNulls());
  EXPECT_THAT(
      vector->type()->asRow().names(), ::testing::ElementsAre("c0", "c1"));
}

TEST_F(VectorFuzzerTest, containerHasNulls) {
  auto countNulls = [](const VectorPtr& vec) {
    if (!vec->nulls()) {
      return 0;
    }
    return BaseVector::countNulls(vec->nulls(), vec->size());
  };

  VectorFuzzer::Options opts;
  opts.vectorSize = 1000;
  opts.nullRatio = 0.5;
  opts.normalizeMapKeys = false;
  opts.containerHasNulls = true;
  opts.allowDictionaryVector = false;
  opts.allowConstantVector = false;

  {
    VectorFuzzer fuzzer(opts, pool());

    auto arrayVector = fuzzer.fuzz(ARRAY(BIGINT()));
    auto mapVector = fuzzer.fuzz(MAP(BIGINT(), BIGINT()));
    auto rowVector = fuzzer.fuzz(ROW({BIGINT(), BIGINT()}));

    // Check that both top level and elements have nulls.
    EXPECT_GT(countNulls(arrayVector), 0);
    EXPECT_GT(countNulls(mapVector), 0);
    EXPECT_GT(countNulls(rowVector), 0);

    auto arrayElements = arrayVector->as<ArrayVector>()->elements();
    auto mapKeys = mapVector->as<MapVector>()->mapKeys();
    auto mapValues = mapVector->as<MapVector>()->mapValues();
    auto rowCol0 = rowVector->as<RowVector>()->childAt(0);
    auto rowCol1 = rowVector->as<RowVector>()->childAt(1);

    EXPECT_GT(countNulls(arrayElements), 0);
    EXPECT_GT(countNulls(mapKeys), 0);
    EXPECT_GT(countNulls(mapValues), 0);
    EXPECT_GT(countNulls(rowCol0), 0);
    EXPECT_GT(countNulls(rowCol1), 0);
  }

  // Test with containerHasNulls false.
  {
    opts.containerHasNulls = false;
    VectorFuzzer fuzzer(opts, pool());

    auto arrayVector = fuzzer.fuzz(ARRAY(BIGINT()));
    auto mapVector = fuzzer.fuzz(MAP(BIGINT(), BIGINT()));
    auto rowVector = fuzzer.fuzz(ROW({BIGINT(), BIGINT()}));

    // Check that both top level and elements have nulls.
    EXPECT_GT(countNulls(arrayVector), 0);
    EXPECT_GT(countNulls(mapVector), 0);
    EXPECT_GT(countNulls(rowVector), 0);

    auto arrayElements = arrayVector->as<ArrayVector>()->elements();
    auto mapKeys = mapVector->as<MapVector>()->mapKeys();
    auto mapValues = mapVector->as<MapVector>()->mapValues();
    auto rowCol0 = rowVector->as<RowVector>()->childAt(0);
    auto rowCol1 = rowVector->as<RowVector>()->childAt(1);

    EXPECT_EQ(countNulls(arrayElements), 0);
    EXPECT_EQ(countNulls(mapKeys), 0);
    EXPECT_EQ(countNulls(mapValues), 0);
    EXPECT_EQ(countNulls(rowCol0), 0);
    EXPECT_EQ(countNulls(rowCol1), 0);
  }

  // Test with containerHasNulls false. Flat vector version.
  {
    opts.containerHasNulls = false;
    VectorFuzzer fuzzer(opts, pool());

    auto arrayVector = fuzzer.fuzzFlat(ARRAY(BIGINT()));
    auto mapVector = fuzzer.fuzzFlat(MAP(BIGINT(), BIGINT()));
    auto rowVector = fuzzer.fuzzFlat(ROW({BIGINT(), BIGINT()}));

    // Check that both top level and elements have nulls.
    EXPECT_GT(countNulls(arrayVector), 0);
    EXPECT_GT(countNulls(mapVector), 0);
    EXPECT_GT(countNulls(rowVector), 0);

    auto arrayElements = arrayVector->as<ArrayVector>()->elements();
    auto mapKeys = mapVector->as<MapVector>()->mapKeys();
    auto mapValues = mapVector->as<MapVector>()->mapValues();
    auto rowCol0 = rowVector->as<RowVector>()->childAt(0);
    auto rowCol1 = rowVector->as<RowVector>()->childAt(1);

    EXPECT_EQ(countNulls(arrayElements), 0);
    EXPECT_EQ(countNulls(mapKeys), 0);
    EXPECT_EQ(countNulls(mapValues), 0);
    EXPECT_EQ(countNulls(rowCol0), 0);
    EXPECT_EQ(countNulls(rowCol1), 0);
  }
}

FlatVectorPtr<Timestamp> genTimestampVector(
    VectorFuzzer::Options::TimestampPrecision precision,
    size_t vectorSize,
    memory::MemoryPool* pool) {
  VectorFuzzer::Options opts;
  opts.vectorSize = vectorSize;
  opts.timestampPrecision = precision;

  VectorFuzzer fuzzer(opts, pool);
  return std::dynamic_pointer_cast<FlatVector<Timestamp>>(
      fuzzer.fuzzFlat(TIMESTAMP()));
};

TEST_F(VectorFuzzerTest, timestamp) {
  const size_t vectorSize = 1000;

  // Second granularity.
  auto secTsVector = genTimestampVector(
      VectorFuzzer::Options::TimestampPrecision::kSeconds, vectorSize, pool());

  for (size_t i = 0; i < vectorSize; ++i) {
    auto ts = secTsVector->valueAt(i);
    ASSERT_EQ(ts.getNanos(), 0);
  }

  // Millisecond granularity.
  auto milliTsVector = genTimestampVector(
      VectorFuzzer::Options::TimestampPrecision::kMilliSeconds,
      vectorSize,
      pool());

  for (size_t i = 0; i < vectorSize; ++i) {
    auto ts = milliTsVector->valueAt(i);
    ASSERT_EQ(ts.getNanos() % 1'000'000, 0);
  }

  // Microsecond granularity.
  auto microTsVector = genTimestampVector(
      VectorFuzzer::Options::TimestampPrecision::kMicroSeconds,
      vectorSize,
      pool());

  for (size_t i = 0; i < vectorSize; ++i) {
    auto ts = microTsVector->valueAt(i);
    ASSERT_EQ(ts.getNanos() % 1'000, 0);
  }

  // Nanosecond granularity.
  auto nanoTsVector = genTimestampVector(
      VectorFuzzer::Options::TimestampPrecision::kNanoSeconds,
      vectorSize,
      pool());

  // Check that at least one timestamp has nano > 0.
  bool nanosFound = false;
  for (size_t i = 0; i < vectorSize; ++i) {
    if (nanoTsVector->valueAt(i).getNanos() > 0) {
      nanosFound = true;
    }
  }
  ASSERT_TRUE(nanosFound);
}

TEST_F(VectorFuzzerTest, assorted) {
  VectorFuzzer::Options opts;
  VectorFuzzer fuzzer(opts, pool());

  auto vector = fuzzer.fuzzMap(
      fuzzer.fuzzDictionary(fuzzer.fuzzFlat(INTEGER(), 10), 100),
      fuzzer.fuzzArray(fuzzer.fuzzConstant(DOUBLE(), 40), 100),
      10);
  ASSERT_TRUE(vector->type()->kindEquals(MAP(INTEGER(), ARRAY(DOUBLE()))));

  // Cast map.
  ASSERT_EQ(VectorEncoding::Simple::MAP, vector->encoding());
  auto map = vector->as<MapVector>();

  // Cast map key.
  ASSERT_EQ(VectorEncoding::Simple::DICTIONARY, map->mapKeys()->encoding());
  auto key =
      map->mapKeys()
          ->as<DictionaryVector<TypeTraits<TypeKind::INTEGER>::NativeType>>();
  ASSERT_EQ(VectorEncoding::Simple::FLAT, key->valueVector()->encoding());

  // Cast map value.
  ASSERT_EQ(VectorEncoding::Simple::ARRAY, map->mapValues()->encoding());
  auto value = map->mapValues()->as<ArrayVector>();

  ASSERT_EQ(VectorEncoding::Simple::CONSTANT, value->elements()->encoding());
}

TEST_F(VectorFuzzerTest, randomized) {
  VectorFuzzer::Options opts;
  opts.allowLazyVector = true;
  opts.nullRatio = 0.5;
  VectorFuzzer fuzzer(opts, pool());

  for (size_t i = 0; i < 50; ++i) {
    auto type = fuzzer.randType();

    if (i % 2 == 0) {
      auto vector = fuzzer.fuzz(type);
      ASSERT_TRUE(vector->type()->kindEquals(type));
    } else {
      auto vector = fuzzer.fuzzNotNull(type);
      ASSERT_TRUE(vector->type()->kindEquals(type));
      ASSERT_FALSE(vector->loadedVector()->mayHaveNulls());
    }
  }
}

void assertEqualVectors(
    SelectivityVector* rowsToCompare,
    const VectorPtr& expected,
    const VectorPtr& actual) {
  ASSERT_LE(rowsToCompare->end(), actual->size())
      << "Vectors should at least have the required amount of rows that need "
         "to be verified.";
  ASSERT_TRUE(expected->type()->equivalent(*actual->type()))
      << "Expected " << expected->type()->toString() << ", but got "
      << actual->type()->toString();
  rowsToCompare->applyToSelected([&](vector_size_t i) {
    ASSERT_TRUE(expected->equalValueAt(actual.get(), i, i))
        << "at " << i << ": expected " << expected->toString(i) << ", but got "
        << actual->toString(i);
  });
}

TEST_F(VectorFuzzerTest, lazyOverFlat) {
  // Verify that lazy vectors generated from flat vectors are loaded properly.
  VectorFuzzer::Options opts;
  SelectivityVector partialRows(opts.vectorSize);
  // non-nullable
  {
    VectorFuzzer fuzzer(opts, pool());
    // Start with 1 to ensure at least one row is selected.
    for (int i = 1; i < opts.vectorSize; ++i) {
      if (fuzzer.coinToss(0.6)) {
        partialRows.setValid(i, false);
      }
    }
    partialRows.updateBounds();
    auto vector = fuzzer.fuzzConstant(INTEGER());
    auto lazy = VectorFuzzer::wrapInLazyVector(vector);
    LazyVector::ensureLoadedRows(lazy, partialRows);
    assertEqualVectors(&partialRows, lazy, vector);

    vector = fuzzer.fuzzFlat(BIGINT());
    lazy = VectorFuzzer::wrapInLazyVector(vector);
    LazyVector::ensureLoadedRows(lazy, partialRows);
    assertEqualVectors(&partialRows, lazy, vector);

    vector = fuzzer.fuzzFlat(ARRAY(BIGINT()));
    lazy = VectorFuzzer::wrapInLazyVector(vector);
    LazyVector::ensureLoadedRows(lazy, partialRows);
    assertEqualVectors(&partialRows, lazy, vector);

    vector = fuzzer.fuzzFlat(MAP(BIGINT(), INTEGER()));
    lazy = VectorFuzzer::wrapInLazyVector(vector);
    LazyVector::ensureLoadedRows(lazy, partialRows);
    assertEqualVectors(&partialRows, lazy, vector);
  }
  // nullable
  {
    opts.nullRatio = 0.5;
    VectorFuzzer fuzzer(opts, pool());

    auto vector = fuzzer.fuzzConstant(INTEGER());
    auto lazy = VectorFuzzer::wrapInLazyVector(vector);
    LazyVector::ensureLoadedRows(lazy, partialRows);
    assertEqualVectors(&partialRows, lazy, vector);

    vector = fuzzer.fuzzFlat(BIGINT());
    lazy = VectorFuzzer::wrapInLazyVector(vector);
    LazyVector::ensureLoadedRows(lazy, partialRows);
    assertEqualVectors(&partialRows, lazy, vector);

    vector = fuzzer.fuzzFlat(ARRAY(BIGINT()));
    lazy = VectorFuzzer::wrapInLazyVector(vector);
    LazyVector::ensureLoadedRows(lazy, partialRows);
    assertEqualVectors(&partialRows, lazy, vector);

    vector = fuzzer.fuzzFlat(MAP(BIGINT(), INTEGER()));
    lazy = VectorFuzzer::wrapInLazyVector(vector);
    LazyVector::ensureLoadedRows(lazy, partialRows);
    assertEqualVectors(&partialRows, lazy, vector);
  }
}

TEST_F(VectorFuzzerTest, lazyOverDictionary) {
  // Verify that when lazy vectors generated over dictionary vectors are loaded,
  // the resulting loaded vector retains dictionary wrapping.
  VectorFuzzer::Options opts;
  opts.nullRatio = 0.3;
  SelectivityVector partialRows(opts.vectorSize);
  VectorFuzzer fuzzer(opts, pool());
  // Starting from 1 to ensure at least one row is selected.
  for (int i = 1; i < opts.vectorSize; ++i) {
    if (fuzzer.coinToss(0.7)) {
      partialRows.setValid(i, false);
    }
  }
  partialRows.updateBounds();

  // Case 1: Applying a single dictionary layer.
  auto vector = fuzzer.fuzzFlat(BIGINT());
  auto dict = fuzzer.fuzzDictionary(vector);
  auto lazy = VectorFuzzer::wrapInLazyVector(dict);
  LazyVector::ensureLoadedRows(lazy, partialRows);
  ASSERT_TRUE(VectorEncoding::isDictionary(lazy->loadedVector()->encoding()));
  assertEqualVectors(&partialRows, dict, lazy);

  partialRows.applyToSelected([&](vector_size_t row) {
    ASSERT_EQ(dict->isNullAt(row), lazy->isNullAt(row));
    if (!lazy->isNullAt(row)) {
      ASSERT_EQ(dict->wrappedIndex(row), lazy->wrappedIndex(row));
    }
  });

  // Case 2: Applying multiple (3 layers) dictionary layers.
  dict = fuzzer.fuzzDictionary(vector);
  dict = fuzzer.fuzzDictionary(dict);
  dict = fuzzer.fuzzDictionary(dict);
  lazy = VectorFuzzer::wrapInLazyVector(dict);

  // Also verify that the lazy layer is applied on the innermost dictionary
  // layer. Should look like Dict(Dict(Dict(Lazy(Base)))))
  ASSERT_TRUE(VectorEncoding::isDictionary(lazy->encoding()));
  ASSERT_TRUE(VectorEncoding::isDictionary(lazy->valueVector()->encoding()));
  ASSERT_TRUE(
      VectorEncoding::isLazy(lazy->valueVector()->valueVector()->encoding()));
  LazyVector::ensureLoadedRows(lazy, partialRows);
  ASSERT_TRUE(VectorEncoding::isDictionary(lazy->loadedVector()->encoding()));
  assertEqualVectors(&partialRows, dict, lazy);

  partialRows.applyToSelected([&](vector_size_t row) {
    ASSERT_EQ(dict->isNullAt(row), lazy->isNullAt(row));
    if (!lazy->isNullAt(row)) {
      ASSERT_EQ(dict->wrappedIndex(row), lazy->wrappedIndex(row));
    }
  });
}

TEST_F(VectorFuzzerTest, fuzzRowChildrenToLazy) {
  VectorFuzzer::Options opts;
  opts.nullRatio = 0.3;
  VectorFuzzer fuzzer(opts, pool());
  // Fuzz row with flat children for easy lazy encoding checks.
  auto children = {
      fuzzer.fuzzFlat(INTEGER()),
      fuzzer.fuzzFlat(INTEGER()),
      fuzzer.fuzzFlat(INTEGER())};
  auto inputRow = std::make_shared<RowVector>(
      pool(),
      ROW({INTEGER(), INTEGER(), INTEGER()}),
      nullptr,
      opts.vectorSize,
      std::move(children));
  ASSERT_FALSE(inputRow->childAt(0)->isLazy());
  ASSERT_FALSE(inputRow->childAt(1)->isLazy());
  ASSERT_FALSE(inputRow->childAt(2)->isLazy());
  // Child 0: Lazy not loaded
  // Child 1: Lazy and loaded
  // Child 2: Not Lazy
  std::vector<int> columnsToWrapInLazy = {0, -1};
  auto wrappedRow = fuzzer.fuzzRowChildrenToLazy(inputRow, columnsToWrapInLazy);
  ASSERT_TRUE(wrappedRow->childAt(0)->isLazy());
  ASSERT_TRUE(wrappedRow->childAt(1)->isLazy());
  ASSERT_FALSE(inputRow->childAt(2)->isLazy());
  ASSERT_FALSE(wrappedRow->childAt(0)->as<LazyVector>()->isLoaded());
  ASSERT_TRUE(wrappedRow->childAt(1)->as<LazyVector>()->isLoaded());
}

TEST_F(VectorFuzzerTest, flatInputRow) {
  VectorFuzzer fuzzer({.vectorSize = 10}, pool());
  auto vector = fuzzer.fuzzInputFlatRow(
      ROW({DOUBLE(), ARRAY(BIGINT()), MAP(BIGINT(), VARCHAR())}));
  ASSERT_TRUE(vector->type()->kindEquals(
      ROW({DOUBLE(), ARRAY(BIGINT()), MAP(BIGINT(), VARCHAR())})));
  ASSERT_EQ(VectorEncoding::Simple::FLAT, vector->childAt(0)->encoding());
  ASSERT_EQ(VectorEncoding::Simple::ARRAY, vector->childAt(1)->encoding());
  ASSERT_EQ(VectorEncoding::Simple::MAP, vector->childAt(2)->encoding());

  // Arrays.
  auto elements = vector->childAt(1)->as<ArrayVector>()->elements();
  ASSERT_TRUE(elements->type()->kindEquals(BIGINT()));
  ASSERT_EQ(VectorEncoding::Simple::FLAT, elements->encoding());

  // Maps.
  auto mapKeys = vector->childAt(2)->as<MapVector>()->mapKeys();
  ASSERT_TRUE(mapKeys->type()->kindEquals(BIGINT()));
  ASSERT_EQ(VectorEncoding::Simple::FLAT, mapKeys->encoding());

  auto mapValues = vector->childAt(2)->as<MapVector>()->mapValues();
  ASSERT_TRUE(mapValues->type()->kindEquals(VARCHAR()));
  ASSERT_EQ(VectorEncoding::Simple::FLAT, mapValues->encoding());
}

void VectorFuzzerTest::validateMaxSizes(VectorPtr vector, size_t maxSize) {
  if (vector->typeKind() == TypeKind::ARRAY) {
    validateMaxSizes(vector->template as<ArrayVector>()->elements(), maxSize);
  } else if (vector->typeKind() == TypeKind::MAP) {
    auto mapVector = vector->as<MapVector>();
    validateMaxSizes(mapVector->mapKeys(), maxSize);
    validateMaxSizes(mapVector->mapValues(), maxSize);
  } else if (vector->typeKind() == TypeKind::ROW) {
    auto rowVector = vector->as<RowVector>();
    for (const auto& child : rowVector->children()) {
      validateMaxSizes(child, maxSize);
    }
  }
  EXPECT_LE(vector->size(), maxSize);
}

// Ensures we don't generate inner vectors exceeding `complexElementsMaxSize`.
TEST_F(VectorFuzzerTest, complexTooLarge) {
  VectorFuzzer::Options opts;
  VectorFuzzer fuzzer(opts, pool());
  VectorPtr vector;

  vector = fuzzer.fuzzFlat(ARRAY(ARRAY(ARRAY(ARRAY(ARRAY(SMALLINT()))))));
  validateMaxSizes(vector, opts.complexElementsMaxSize);

  vector = fuzzer.fuzzFlat(MAP(
      BIGINT(), MAP(SMALLINT(), MAP(INTEGER(), MAP(SMALLINT(), DOUBLE())))));
  validateMaxSizes(vector, opts.complexElementsMaxSize);

  vector = fuzzer.fuzzFlat(
      ROW({BIGINT(), ROW({SMALLINT(), ROW({INTEGER()})}), DOUBLE()}));
  validateMaxSizes(vector, opts.complexElementsMaxSize);

  // Mix and match.
  vector = fuzzer.fuzzFlat(
      ROW({ARRAY(ROW({SMALLINT(), ROW({MAP(INTEGER(), DOUBLE())})}))}));
  validateMaxSizes(vector, opts.complexElementsMaxSize);

  // Try a more restrictive max size.
  opts.complexElementsMaxSize = 100;
  fuzzer.setOptions(opts);

  vector = fuzzer.fuzzFlat(ARRAY(ARRAY(ARRAY(ARRAY(ARRAY(SMALLINT()))))));
  validateMaxSizes(vector, opts.complexElementsMaxSize);

  // If opts.containerVariableLength is false,  then throw if requested size
  // cant be satisfied.
  opts.containerVariableLength = false;
  fuzzer.setOptions(opts);

  EXPECT_THROW(
      fuzzer.fuzzFlat(ARRAY(ARRAY(ARRAY(ARRAY(ARRAY(SMALLINT())))))),
      VeloxUserError);
}

TEST_F(VectorFuzzerTest, randOrderableType) {
  VectorFuzzer::Options opts;
  VectorFuzzer fuzzer(opts, pool());
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(fuzzer.randOrderableType()->isOrderable());
  }
}

TEST_F(VectorFuzzerTest, randMapType) {
  VectorFuzzer::Options opts;
  VectorFuzzer fuzzer(opts, pool());
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(fuzzer.randMapType()->isMap());
  }
}

TEST_F(VectorFuzzerTest, randTypeByWidth) {
  VectorFuzzer::Options opts;
  VectorFuzzer fuzzer(opts, pool());

  // Test typeWidth.
  TypePtr type = BIGINT();
  ASSERT_EQ(approximateTypeEncodingwidth(type), 1);
  type = ARRAY(BIGINT());
  ASSERT_EQ(approximateTypeEncodingwidth(type), 2);
  type = MAP(BIGINT(), ARRAY(VARCHAR()));
  ASSERT_EQ(approximateTypeEncodingwidth(type), 4);
  type = ROW(
      {INTEGER(), ARRAY(BIGINT()), MAP(VARCHAR(), DOUBLE()), ROW({TINYINT()})});
  ASSERT_EQ(approximateTypeEncodingwidth(type), 7);

  // Test randType by width. Results should be at least a RowType with one
  // field, so the minimal type width is 2.
  type = fuzzer.randRowTypeByWidth(-1);
  ASSERT_GE(approximateTypeEncodingwidth(type), 2);
  type = fuzzer.randRowTypeByWidth(0);
  ASSERT_GE(approximateTypeEncodingwidth(type), 2);
  type = fuzzer.randRowTypeByWidth(1);
  ASSERT_GE(approximateTypeEncodingwidth(type), 2);

  folly::Random::DefaultGenerator rng;
  rng.seed(0);
  for (auto i = 0; i < 1000; ++i) {
    const auto width = folly::Random::rand32(rng) % 128;
    type = fuzzer.randRowTypeByWidth(width);
    ASSERT_GE(approximateTypeEncodingwidth(type), width);
  }
}

TEST_F(VectorFuzzerTest, customTypeGenerator) {
  // Verify that the fuzzer automatically pick up the custom type generator for
  // a registered custom type. In this case, we pick json type for verification.
  VectorFuzzer::Options opts;
  VectorFuzzer fuzzer(opts, pool());

  const uint32_t kSize = 10;
  auto verifyJson = [&](const VectorPtr& result) {
    EXPECT_TRUE(result != nullptr);
    EXPECT_TRUE(isJsonType(result->type()));
    EXPECT_EQ(result->size(), kSize);

    DecodedVector decoded;
    decoded.decode(*result, SelectivityVector(kSize));
    folly::dynamic json;
    folly::json::serialization_opts opts;
    opts.allow_non_string_keys = true;
    opts.allow_nan_inf = true;
    for (auto j = 0; j < kSize; ++j) {
      if (decoded.isNullAt(j)) {
        continue;
      }
      std::string value = decoded.valueAt<StringView>(j);
      try {
        json = folly::parseJson(value, opts);
      } catch (...) {
        EXPECT_TRUE(false);
      }
    }
  };
  for (int i = 0; i < 5; i++) {
    // We verify all the public APIs of the fuzzer.
    verifyJson(fuzzer.fuzz(JSON(), kSize));
    verifyJson(fuzzer.fuzzNotNull(JSON(), kSize));
    verifyJson(fuzzer.fuzzConstant(JSON(), kSize));
    verifyJson(fuzzer.fuzzFlat(JSON(), kSize));
    verifyJson(fuzzer.fuzzFlatNotNull(JSON(), kSize));
  }
}

TEST_F(VectorFuzzerTest, jsonConstrained) {
  VectorFuzzer::Options opts;
  VectorFuzzer fuzzer(opts, pool());

  const TypePtr type = ARRAY(ROW({BIGINT()}));
  std::shared_ptr<JsonInputGenerator> generator =
      std::make_shared<JsonInputGenerator>(
          0,
          JSON(),
          0.2,
          std::make_unique<RandomInputGenerator<ArrayType>>(0, type, 0.3));

  const uint32_t kSize = 1000;
  const auto& jsonOpts = generator->serializationOptions();
  DecodedVector decoded;
  for (auto i = 0; i < 10; ++i) {
    auto vector = fuzzer.fuzz(JSON(), kSize, generator);
    VELOX_CHECK_NOT_NULL(vector);
    VELOX_CHECK_EQ(vector->type()->kind(), TypeKind::VARCHAR);
    decoded.decode(*vector, SelectivityVector(kSize));
    for (auto j = 0; j < kSize; ++j) {
      if (decoded.isNullAt(j)) {
        continue;
      }
      std::string value = decoded.valueAt<StringView>(j);
      folly::dynamic json;
      EXPECT_NO_THROW(json = folly::parseJson(value, jsonOpts));
      EXPECT_TRUE(json.isNull() || json.isArray());
    }
  }
}

TEST_F(VectorFuzzerTest, setConstrained) {
  VectorFuzzer::Options opts;
  VectorFuzzer fuzzer(opts, pool());

  std::shared_ptr<SetConstrainedGenerator> generator =
      std::make_shared<SetConstrainedGenerator>(
          0, VARCHAR(), std::vector<variant>{variant("a"), variant("b")});
  const uint32_t kSize = 1000;
  auto vector = fuzzer.fuzz(VARCHAR(), kSize, generator);

  DecodedVector decoded(*vector, SelectivityVector(kSize));
  for (auto i = 0; i < kSize; ++i) {
    std::string value = decoded.valueAt<StringView>(i);
    EXPECT_TRUE(value == "a" || value == "b");
  }
}

TEST_F(VectorFuzzerTest, qdigestTypeGeneration) {
  VectorFuzzer::Options opts;
  opts.nullRatio = 0.2;
  opts.vectorSize = 10;
  VectorFuzzer fuzzer(opts, pool());

  const auto qdigestRealType = QDIGEST(REAL());
  const auto qdigestDoulbeType = QDIGEST(DOUBLE());
  const auto qdigestBigIntType = QDIGEST(BIGINT());

  // Test QDigest with BIGINT parameter
  {
    auto vector = fuzzer.fuzz(QDIGEST(BIGINT()));
    ASSERT_NE(vector, nullptr);
    EXPECT_TRUE(vector->type()->equivalent(*qdigestBigIntType));
    EXPECT_EQ(vector->size(), opts.vectorSize);
  }

  // Test QDigest with REAL parameter
  {
    auto vector = fuzzer.fuzz(QDIGEST(REAL()));
    ASSERT_NE(vector, nullptr);
    EXPECT_TRUE(vector->type()->equivalent(*qdigestRealType));
    EXPECT_EQ(vector->size(), opts.vectorSize);
  }

  // Test QDigest with DOUBLE parameter
  {
    auto vector = fuzzer.fuzz(QDIGEST(BIGINT()));
    ASSERT_NE(vector, nullptr);
    EXPECT_TRUE(vector->type()->equivalent(*qdigestBigIntType));
    EXPECT_EQ(vector->size(), opts.vectorSize);
  }
}
} // namespace
