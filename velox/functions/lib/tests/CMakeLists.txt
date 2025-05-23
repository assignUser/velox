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
add_executable(
  velox_functions_lib_test
  ApproxMostFrequentStreamSummaryTest.cpp
  ArrayRemoveNullsTest.cpp
  CheckNestedNullsTest.cpp
  DateTimeFormatterTest.cpp
  IsNullTest.cpp
  IsNotNullTest.cpp
  KllSketchTest.cpp
  LambdaFunctionUtilTest.cpp
  MapConcatTest.cpp
  QuantileDigestTest.cpp
  Re2FunctionsTest.cpp
  RepeatTest.cpp
  TDigestTest.cpp
  TimeUtilsTest.cpp
  Utf8Test.cpp
  ZetaDistributionTest.cpp)

add_test(
  NAME velox_functions_lib_test
  COMMAND velox_functions_lib_test
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(
  velox_functions_lib_test
  velox_functions_lib
  velox_functions_test_lib
  velox_is_null_functions
  velox_exec_test_lib
  velox_expression
  velox_memory
  velox_dwio_common_test_utils
  GTest::gtest
  GTest::gtest_main
  GTest::gmock)
