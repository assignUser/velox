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
velox_add_library(
  velox_exec
  AddressableNonNullValueList.cpp
  Aggregate.cpp
  AggregateCompanionAdapter.cpp
  AggregateCompanionSignatures.cpp
  AggregateFunctionRegistry.cpp
  AggregateInfo.cpp
  AggregationMasks.cpp
  AggregateWindow.cpp
  ArrowStream.cpp
  AssignUniqueId.cpp
  CallbackSink.cpp
  ContainerRowSerde.cpp
  DistinctAggregations.cpp
  Driver.cpp
  EnforceSingleRow.cpp
  Exchange.cpp
  ExchangeClient.cpp
  ExchangeQueue.cpp
  ExchangeSource.cpp
  Expand.cpp
  FilterProject.cpp
  GroupId.cpp
  GroupingSet.cpp
  HashAggregation.cpp
  HashBuild.cpp
  HashJoinBridge.cpp
  HashPartitionFunction.cpp
  HashProbe.cpp
  HashTable.cpp
  IndexLookupJoin.cpp
  JoinBridge.cpp
  Limit.cpp
  LocalPartition.cpp
  LocalPlanner.cpp
  MarkDistinct.cpp
  MemoryReclaimer.cpp
  Merge.cpp
  MergeJoin.cpp
  MergeSource.cpp
  NestedLoopJoinBuild.cpp
  NestedLoopJoinProbe.cpp
  Operator.cpp
  OperatorUtils.cpp
  OrderBy.cpp
  OutputBuffer.cpp
  OutputBufferManager.cpp
  OperatorTraceReader.cpp
  OperatorTraceScan.cpp
  OperatorTraceWriter.cpp
  ParallelProject.cpp
  TaskTraceReader.cpp
  TaskTraceWriter.cpp
  Trace.cpp
  TraceUtil.cpp
  PartitionedOutput.cpp
  PartitionFunction.cpp
  PartitionStreamingWindowBuild.cpp
  PlanNodeStats.cpp
  PrefixSort.cpp
  ProbeOperatorState.cpp
  RowsStreamingWindowBuild.cpp
  RowContainer.cpp
  RowNumber.cpp
  ScaledScanController.cpp
  ScaleWriterLocalPartition.cpp
  SortBuffer.cpp
  SortedAggregations.cpp
  SortWindowBuild.cpp
  Spill.cpp
  SpillFile.cpp
  Spiller.cpp
  StreamingAggregation.cpp
  Strings.cpp
  TableScan.cpp
  TableWriteMerge.cpp
  TableWriter.cpp
  Task.cpp
  TopN.cpp
  TopNRowNumber.cpp
  Unnest.cpp
  Values.cpp
  VectorHasher.cpp
  Window.cpp
  WindowBuild.cpp
  WindowFunction.cpp
  WindowPartition.cpp)

velox_link_libraries(
  velox_exec
  velox_file
  velox_core
  velox_vector
  velox_connector
  velox_expression
  velox_time
  velox_common_base
  velox_test_util
  velox_arrow_bridge
  velox_common_compression)

velox_add_library(velox_cursor Cursor.cpp)
velox_link_libraries(
  velox_cursor
  velox_core
  velox_exception
  velox_expression
  velox_dwio_common
  velox_dwio_dwrf_reader
  velox_dwio_dwrf_writer
  velox_type_fbhive
  velox_presto_serializer
  velox_functions_prestosql
  velox_aggregates)

if(${VELOX_BUILD_TESTING})
  add_subdirectory(fuzzer)
  add_subdirectory(tests)
elseif(${VELOX_BUILD_TEST_UTILS})
  add_subdirectory(tests/utils)
endif()

if(${VELOX_ENABLE_BENCHMARKS})
  add_subdirectory(benchmarks)
endif()

add_subdirectory(prefixsort)
