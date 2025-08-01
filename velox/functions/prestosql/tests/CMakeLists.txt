# Copyright (c) Facebook, Inc. and its affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

add_subdirectory(utils)

add_executable(
  velox_functions_test
  ArithmeticTest.cpp
  ArrayAllMatchTest.cpp
  ArrayAnyMatchTest.cpp
  ArrayAverageTest.cpp
  ArrayCombinationsTest.cpp
  ArrayConcatTest.cpp
  ArrayConstructorTest.cpp
  ArrayContainsTest.cpp
  ArrayCumSumTest.cpp
  ArrayDistinctTest.cpp
  ArrayDuplicatesTest.cpp
  ArrayExceptTest.cpp
  ArrayFilterTest.cpp
  ArrayFrequencyTest.cpp
  ArrayFlattenTest.cpp
  ArrayHasDuplicatesTest.cpp
  ArrayIntersectTest.cpp
  ArrayJoinTest.cpp
  ArrayMaxTest.cpp
  ArrayMinTest.cpp
  ArrayMinMaxByTest.cpp
  ArrayNGramsTest.cpp
  ArrayNoneMatchTest.cpp
  ArrayNormalizeTest.cpp
  ArrayPositionTest.cpp
  ArrayRemoveTest.cpp
  ArrayShuffleTest.cpp
  ArraySortTest.cpp
  ArrayTopNTest.cpp
  ArraysOverlapTest.cpp
  ArraySumTest.cpp
  ArrayTrimTest.cpp
  ArrayUnionTest.cpp
  ArgTypesGeneratorTest.cpp
  BinaryFunctionsTest.cpp
  BingTileCastTest.cpp
  BitwiseTest.cpp
  CardinalityTest.cpp
  CeilFloorTest.cpp
  ComparisonsTest.cpp
  DateTimeFunctionsTest.cpp
  DecimalArithmeticTest.cpp
  DistanceFunctionsTest.cpp
  ElementAtTest.cpp
  FailTest.cpp
  FindFirstTest.cpp
  FloatingPointFunctionsTest.cpp
  FromUtf8Test.cpp
  GreatestLeastTest.cpp
  HyperLogLogCastTest.cpp
  HyperLogLogFunctionsTest.cpp
  InPredicateTest.cpp
  IntegerFunctionsTest.cpp
  IPAddressCastTest.cpp
  IPPrefixCastTest.cpp
  JsonCastTest.cpp
  JsonExtractScalarTest.cpp
  JsonFunctionsTest.cpp
  MapEntriesTest.cpp
  MapFilterTest.cpp
  MapFromEntriesTest.cpp
  MapNormalizeTest.cpp
  MapTopNTest.cpp
  MapTopNKeysTest.cpp
  MapTopNValuesTest.cpp
  MapKeysAndValuesTest.cpp
  MapKeysByTopNValuesTest.cpp
  MapMatchTest.cpp
  MapTest.cpp
  MapZipWithTest.cpp
  MultimapFromEntriesTest.cpp
  NotTest.cpp
  ParsePrestoDataSizeTest.cpp
  ProbabilityTest.cpp
  RandTest.cpp
  ReduceTest.cpp
  RegexpSplitTest.cpp
  RegexpReplaceTest.cpp
  ReverseTest.cpp
  RoundTest.cpp
  ScalarFunctionRegTest.cpp
  SequenceTest.cpp
  SfmSketchFunctionsTest.cpp
  SimpleComparisonMatcherTest.cpp
  SliceTest.cpp
  SplitTest.cpp
  SplitToMapTest.cpp
  SplitToMultiMapTest.cpp
  StringFunctionsTest.cpp
  TimestampWithTimeZoneCastTest.cpp
  TransformKeysTest.cpp
  TransformTest.cpp
  TransformValuesTest.cpp
  TrimFunctionsTest.cpp
  TypeOfTest.cpp
  TDigestCastTest.cpp
  QDigestCastTest.cpp
  QDigestFunctionsTest.cpp
  URLFunctionsTest.cpp
  UuidFunctionsTest.cpp
  WidthBucketArrayTest.cpp
  WordStemTest.cpp
  ZipTest.cpp
  ZipWithTest.cpp)

if(VELOX_ENABLE_GEO)
  target_sources(velox_functions_test PRIVATE GeometryFunctionsTest.cpp)
endif()

if(VELOX_ENABLE_FAISS)
  set_source_files_properties(DistanceFunctionsTest.cpp
                              PROPERTIES COMPILE_DEFINITIONS VELOX_ENABLE_FAISS)
endif()

add_test(velox_functions_test velox_functions_test)

target_link_libraries(
  velox_functions_test
  velox_functions_test_lib
  velox_exec
  velox_expression
  velox_functions_prestosql
  velox_functions_lib
  velox_exec_test_lib
  velox_dwio_common_test_utils
  velox_vector_fuzzer
  velox_expression_fuzzer_test_utility
  velox_expression_fuzzer
  GTest::gtest
  GTest::gtest_main
  GTest::gmock
  GTest::gmock_main)
