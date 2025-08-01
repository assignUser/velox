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

#include "velox/connectors/hive/HivePartitionUtil.h"
#include "velox/vector/SimpleVector.h"

namespace facebook::velox::connector::hive {

#define PARTITION_TYPE_DISPATCH(TEMPLATE_FUNC, typeKind, ...)               \
  [&]() {                                                                   \
    switch (typeKind) {                                                     \
      case TypeKind::BOOLEAN:                                               \
      case TypeKind::TINYINT:                                               \
      case TypeKind::SMALLINT:                                              \
      case TypeKind::INTEGER:                                               \
      case TypeKind::BIGINT:                                                \
      case TypeKind::VARCHAR:                                               \
      case TypeKind::VARBINARY:                                             \
      case TypeKind::TIMESTAMP:                                             \
        return VELOX_DYNAMIC_SCALAR_TYPE_DISPATCH(                          \
            TEMPLATE_FUNC, typeKind, __VA_ARGS__);                          \
      default:                                                              \
        VELOX_UNSUPPORTED(                                                  \
            "Unsupported partition type: {}", mapTypeKindToName(typeKind)); \
    }                                                                       \
  }()

namespace {
template <typename T>
inline std::string makePartitionValueString(T value) {
  return folly::to<std::string>(value);
}

template <>
inline std::string makePartitionValueString(bool value) {
  return value ? "true" : "false";
}

template <>
inline std::string makePartitionValueString(Timestamp value) {
  value.toTimezone(Timestamp::defaultTimezone());
  TimestampToStringOptions options;
  options.dateTimeSeparator = ' ';
  // Set the precision to milliseconds, and enable the skipTrailingZeros match
  // the timestamp precision and truncation behavior of Presto.
  options.precision = TimestampPrecision::kMilliseconds;
  options.skipTrailingZeros = true;

  auto result = value.toString(options);

  // Presto's java.sql.Timestamp.toString() always keeps at least one decimal
  // place even when all fractional seconds are zero.
  // If skipTrailingZeros removed all fractional digits, add back ".0" to match
  // Presto's behavior.
  if (auto dotPos = result.find_last_of('.'); dotPos == std::string::npos) {
    // No decimal point found, add ".0"
    result += ".0";
  }

  return result;
}

template <TypeKind Kind>
std::pair<std::string, std::string> makePartitionKeyValueString(
    const BaseVector* partitionVector,
    vector_size_t row,
    const std::string& name,
    bool isDate) {
  using T = typename TypeTraits<Kind>::NativeType;
  if (partitionVector->as<SimpleVector<T>>()->isNullAt(row)) {
    return std::make_pair(name, "");
  }
  if (isDate) {
    return std::make_pair(
        name,
        DATE()->toString(
            partitionVector->as<SimpleVector<int32_t>>()->valueAt(row)));
  }
  return std::make_pair(
      name,
      makePartitionValueString(
          partitionVector->as<SimpleVector<T>>()->valueAt(row)));
}

} // namespace

std::vector<std::pair<std::string, std::string>> extractPartitionKeyValues(
    const RowVectorPtr& partitionsVector,
    vector_size_t row) {
  std::vector<std::pair<std::string, std::string>> partitionKeyValues;
  for (auto i = 0; i < partitionsVector->childrenSize(); i++) {
    partitionKeyValues.push_back(PARTITION_TYPE_DISPATCH(
        makePartitionKeyValueString,
        partitionsVector->childAt(i)->typeKind(),
        partitionsVector->childAt(i)->loadedVector(),
        row,
        asRowType(partitionsVector->type())->nameOf(i),
        partitionsVector->childAt(i)->type()->isDate()));
  }
  return partitionKeyValues;
}

} // namespace facebook::velox::connector::hive
