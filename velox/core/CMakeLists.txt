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
if(${VELOX_BUILD_TESTING})
  add_subdirectory(tests)
endif()

velox_add_library(
  velox_core
  Expressions.cpp
  ITypedExpr.cpp
  FilterToExpression.cpp
  PlanFragment.cpp
  PlanNode.cpp
  QueryConfig.cpp
  QueryCtx.cpp
  SimpleFunctionMetadata.cpp)

velox_link_libraries(
  velox_core
  PUBLIC velox_arrow_bridge
         velox_caching
         velox_common_config
         velox_connector
         velox_exception
         velox_expression_functions
         velox_memory
         velox_type
         velox_vector
         Boost::headers
         Folly::folly
         fmt::fmt
  PRIVATE velox_encode)
