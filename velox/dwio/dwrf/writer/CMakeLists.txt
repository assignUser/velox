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
  velox_dwio_dwrf_writer
  ColumnWriter.cpp
  FlatMapColumnWriter.cpp
  FlushPolicy.cpp
  LayoutPlanner.cpp
  RatioTracker.cpp
  StatisticsBuilder.cpp
  StatisticsBuilderUtils.cpp
  Writer.cpp
  WriterBase.cpp
  WriterContext.cpp
  WriterSink.cpp)

velox_link_libraries(
  velox_dwio_dwrf_writer
  velox_common_hyperloglog
  velox_dwio_common
  velox_dwio_dwrf_common
  velox_dwio_dwrf_utils
  velox_exec
  velox_process
  velox_time
  velox_vector
  Boost::headers
  lz4::lz4
  zstd::zstd
  ZLIB::ZLIB)
