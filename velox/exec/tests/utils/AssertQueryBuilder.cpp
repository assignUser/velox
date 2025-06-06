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

#include "velox/exec/tests/utils/AssertQueryBuilder.h"
#include "velox/exec/Cursor.h"

namespace facebook::velox::exec::test {

namespace {
/// Returns the plan node ID of the only leaf plan node. Throws if 'root' has
/// multiple leaf nodes.
core::PlanNodeId getOnlyLeafPlanNodeId(const core::PlanNodePtr& root) {
  const auto& sources = root->sources();
  if (sources.empty()) {
    return root->id();
  }

  VELOX_CHECK_EQ(1, sources.size());
  return getOnlyLeafPlanNodeId(sources[0]);
}
} // namespace

AssertQueryBuilder::AssertQueryBuilder(
    const core::PlanNodePtr& plan,
    DuckDbQueryRunner& duckDbQueryRunner)
    : duckDbQueryRunner_{&duckDbQueryRunner} {
  params_.planNode = plan;
}

AssertQueryBuilder::AssertQueryBuilder(DuckDbQueryRunner& duckDbQueryRunner)
    : duckDbQueryRunner_{&duckDbQueryRunner} {}

AssertQueryBuilder::AssertQueryBuilder(const core::PlanNodePtr& plan)
    : duckDbQueryRunner_(nullptr) {
  params_.planNode = plan;
}

AssertQueryBuilder& AssertQueryBuilder::plan(const core::PlanNodePtr& plan) {
  params_.planNode = plan;
  return *this;
}

AssertQueryBuilder& AssertQueryBuilder::maxDrivers(int32_t maxDrivers) {
  params_.maxDrivers = maxDrivers;
  return *this;
}

AssertQueryBuilder& AssertQueryBuilder::maxQueryCapacity(
    int64_t maxQueryCapacity) {
  params_.maxQueryCapacity = maxQueryCapacity;
  return *this;
}

AssertQueryBuilder& AssertQueryBuilder::destination(int32_t destination) {
  params_.destination = destination;
  return *this;
}

AssertQueryBuilder& AssertQueryBuilder::serialExecution(bool serial) {
  if (serial) {
    params_.serialExecution = true;
    executor_ = nullptr;
    return *this;
  }
  params_.serialExecution = false;
  executor_ = newExecutor();
  return *this;
}

AssertQueryBuilder& AssertQueryBuilder::barrierExecution(bool barrier) {
  VELOX_CHECK(!params_.barrierExecution);
  params_.barrierExecution = barrier;
  return *this;
}

AssertQueryBuilder& AssertQueryBuilder::config(
    const std::string& key,
    const std::string& value) {
  configs_[key] = value;
  return *this;
}

AssertQueryBuilder& AssertQueryBuilder::configs(
    const std::unordered_map<std::string, std::string>& values) {
  for (auto& entry : values) {
    configs_[entry.first] = entry.second;
  }
  return *this;
}

AssertQueryBuilder& AssertQueryBuilder::connectorSessionProperty(
    const std::string& connectorId,
    const std::string& key,
    const std::string& value) {
  connectorSessionProperties_[connectorId][key] = value;
  return *this;
}

AssertQueryBuilder& AssertQueryBuilder::connectorSessionProperties(
    const std::unordered_map<
        std::string,
        std::unordered_map<std::string, std::string>>& properties) {
  for (const auto& [connectorId, values] : properties) {
    for (const auto& [key, value] : values) {
      connectorSessionProperty(connectorId, key, value);
    }
  }
  return *this;
}

AssertQueryBuilder& AssertQueryBuilder::split(Split split) {
  this->split(getOnlyLeafPlanNodeId(params_.planNode), std::move(split));
  return *this;
}

AssertQueryBuilder& AssertQueryBuilder::split(
    const core::PlanNodeId& planNodeId,
    Split split) {
  splits_[planNodeId].emplace_back(std::move(split));
  return *this;
}

AssertQueryBuilder& AssertQueryBuilder::splits(std::vector<Split> splits) {
  this->splits(getOnlyLeafPlanNodeId(params_.planNode), std::move(splits));
  return *this;
}

AssertQueryBuilder& AssertQueryBuilder::splits(
    const core::PlanNodeId& planNodeId,
    std::vector<Split> splits) {
  splits_[planNodeId] = std::move(splits);
  return *this;
}

AssertQueryBuilder& AssertQueryBuilder::split(
    const std::shared_ptr<connector::ConnectorSplit>& connectorSplit) {
  split(getOnlyLeafPlanNodeId(params_.planNode), connectorSplit);
  return *this;
}

AssertQueryBuilder& AssertQueryBuilder::split(
    const core::PlanNodeId& planNodeId,
    const std::shared_ptr<connector::ConnectorSplit>& connectorSplit) {
  splits_[planNodeId].emplace_back(
      exec::Split(folly::copy(connectorSplit), -1));
  return *this;
}

AssertQueryBuilder& AssertQueryBuilder::splits(
    const std::vector<std::shared_ptr<connector::ConnectorSplit>>&
        connectorSplits) {
  splits(getOnlyLeafPlanNodeId(params_.planNode), connectorSplits);
  return *this;
}

AssertQueryBuilder& AssertQueryBuilder::splits(
    const core::PlanNodeId& planNodeId,
    const std::vector<std::shared_ptr<connector::ConnectorSplit>>&
        connectorSplits) {
  std::vector<Split> splits;
  for (auto& connectorSplit : connectorSplits) {
    splits.emplace_back(exec::Split(folly::copy(connectorSplit), -1));
  }
  splits_[planNodeId] = std::move(splits);
  return *this;
}

AssertQueryBuilder& AssertQueryBuilder::addSplitWithSequence(
    bool addWithSequence) {
  addSplitWithSequence_ = addWithSequence;
  return *this;
}

std::shared_ptr<Task> AssertQueryBuilder::assertResults(
    const std::string& duckDbSql,
    const std::optional<std::vector<uint32_t>>& sortingKeys) {
  VELOX_CHECK_NOT_NULL(duckDbQueryRunner_);

  auto [cursor, results] = readCursor();
  if (results.empty()) {
    test::assertResults(results, ROW({}, {}), duckDbSql, *duckDbQueryRunner_);
  } else if (sortingKeys.has_value()) {
    test::assertResultsOrdered(
        results,
        asRowType(results[0]->type()),
        duckDbSql,
        *duckDbQueryRunner_,
        sortingKeys.value());
  } else {
    test::assertResults(
        results, asRowType(results[0]->type()), duckDbSql, *duckDbQueryRunner_);
  }
  return cursor->task();
}

std::shared_ptr<Task> AssertQueryBuilder::assertResults(
    const RowVectorPtr& expected) {
  return assertResults(std::vector<RowVectorPtr>{expected});
}

std::shared_ptr<Task> AssertQueryBuilder::assertResults(
    const std::vector<RowVectorPtr>& expected) {
  auto [cursor, results] = readCursor();

  assertEqualResults(expected, results);
  return cursor->task();
}

std::shared_ptr<Task> AssertQueryBuilder::assertEmptyResults() {
  auto [cursor, results] = readCursor();
  test::assertEmptyResults(results);
  return cursor->task();
}

std::shared_ptr<Task> AssertQueryBuilder::assertTypeAndNumRows(
    const TypePtr& expectedType,
    vector_size_t expectedNumRows) {
  auto [cursor, results] = readCursor();

  assertEqualTypeAndNumRows(expectedType, expectedNumRows, results);
  return cursor->task();
}

RowVectorPtr AssertQueryBuilder::copyResults(memory::MemoryPool* pool) {
  std::shared_ptr<Task> unused;
  return copyResults(pool, unused);
}

RowVectorPtr AssertQueryBuilder::copyResults(
    memory::MemoryPool* pool,
    std::shared_ptr<Task>& task) {
  auto [cursor, results] = readCursor();

  if (results.empty()) {
    task = cursor->task();
    return BaseVector::create<RowVector>(
        params_.planNode->outputType(), 0, pool);
  }

  auto totalCount = 0;
  for (const auto& result : results) {
    totalCount += result->size();
  }
  auto copy =
      BaseVector::create<RowVector>(results[0]->type(), totalCount, pool);
  auto copyCount = 0;
  for (const auto& result : results) {
    copy->copy(result.get(), copyCount, 0, result->size());
    copyCount += result->size();
  }

  task = cursor->task();
  return copy;
}

uint64_t AssertQueryBuilder::runWithoutResults(std::shared_ptr<Task>& task) {
  auto [cursor, results] = readCursor();
  uint64_t count = 0;
  for (const auto& result : results) {
    count += result->size();
  }
  task = cursor->task();
  return count;
}

std::pair<std::unique_ptr<TaskCursor>, std::vector<RowVectorPtr>>
AssertQueryBuilder::readCursor() {
  VELOX_CHECK_NOT_NULL(params_.planNode);

  if (!configs_.empty() || !connectorSessionProperties_.empty()) {
    if (params_.queryCtx == nullptr) {
      // NOTE: the destructor of 'executor_' will wait for all the async task
      // activities to finish on AssertQueryBuilder dtor.
      static std::atomic<uint64_t> cursorQueryId{0};
      const std::string queryId =
          fmt::format("TaskCursorQuery_{}", cursorQueryId++);
      auto queryPool = memory::memoryManager()->addRootPool(
          queryId, params_.maxQueryCapacity);
      params_.queryCtx = core::QueryCtx::create(
          executor_.get(),
          core::QueryConfig({}),
          std::
              unordered_map<std::string, std::shared_ptr<config::ConfigBase>>{},
          cache::AsyncDataCache::getInstance(),
          std::move(queryPool),
          nullptr,
          queryId);
    }
  }
  if (!configs_.empty()) {
    params_.queryCtx->testingOverrideConfigUnsafe(std::move(configs_));
  }
  if (!connectorSessionProperties_.empty()) {
    for (auto& [connectorId, sessionProperties] : connectorSessionProperties_) {
      params_.queryCtx->setConnectorSessionOverridesUnsafe(
          connectorId, std::move(sessionProperties));
    }
  }

  return test::readCursor(params_, [&](exec::TaskCursor* taskCursor) {
    if (taskCursor->noMoreSplits()) {
      return;
    }
    auto& task = taskCursor->task();
    VELOX_CHECK(!params_.barrierExecution || params_.serialExecution);
    if (params_.barrierExecution) {
      int numSplits{0};
      for (auto& [nodeId, nodeSplits] : splits_) {
        if (nodeSplits.empty()) {
          task->noMoreSplits(nodeId);
          continue;
        }
        ++numSplits;
        if (addSplitWithSequence_) {
          task->addSplitWithSequence(
              nodeId, std::move(nodeSplits[0]), ++sequenceId_);
          task->setMaxSplitSequenceId(nodeId, sequenceId_);
        } else {
          task->addSplit(nodeId, std::move(nodeSplits[0]));
        }
        nodeSplits.erase(nodeSplits.begin());
      }
      if (numSplits > 0) {
        VELOX_CHECK_EQ(
            numSplits,
            splits_.size(),
            "Barrier task execution mode requires all the sources have the same number of splits");
        task->requestBarrier();
      } else {
        taskCursor->setNoMoreSplits();
      }
    } else {
      for (auto& [nodeId, nodeSplits] : splits_) {
        for (auto& split : nodeSplits) {
          if (addSplitWithSequence_) {
            task->addSplitWithSequence(nodeId, std::move(split), ++sequenceId_);
            task->setMaxSplitSequenceId(nodeId, sequenceId_);
          } else {
            task->addSplit(nodeId, std::move(split));
          }
        }
        task->noMoreSplits(nodeId);
      }
      taskCursor->setNoMoreSplits();
    }
  });
}
} // namespace facebook::velox::exec::test
