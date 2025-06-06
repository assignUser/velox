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

#pragma once

#include <folly/Executor.h>

#include "velox/common/caching/FileGroupStats.h"
#include "velox/common/caching/ScanTracker.h"
#include "velox/common/caching/SsdCache.h"
#include "velox/common/io/IoStatistics.h"
#include "velox/common/io/Options.h"
#include "velox/dwio/common/BufferedInput.h"
#include "velox/dwio/common/CacheInputStream.h"
#include "velox/dwio/common/InputStream.h"

DECLARE_int32(cache_load_quantum);

namespace facebook::velox::dwio::common {

struct CacheRequest {
  CacheRequest(
      cache::RawFileCacheKey _key,
      uint64_t _size,
      cache::TrackingId _trackingId)
      : key(_key), size(_size), trackingId(_trackingId) {}

  cache::RawFileCacheKey key;
  uint64_t size;
  cache::TrackingId trackingId;
  cache::CachePin pin;
  cache::SsdPin ssdPin;

  /// True if this should be coalesced into a CoalescedLoad with other nearby
  /// requests with a similar load probability. This is false for sparsely
  /// accessed large columns where hitting one piece should not load the
  /// adjacent pieces.
  bool coalesces{true};
  const SeekableInputStream* stream;
};

class CachedBufferedInput : public BufferedInput {
 public:
  CachedBufferedInput(
      std::shared_ptr<ReadFile> readFile,
      const MetricsLogPtr& metricsLog,
      StringIdLease fileNum,
      cache::AsyncDataCache* cache,
      std::shared_ptr<cache::ScanTracker> tracker,
      StringIdLease groupId,
      std::shared_ptr<IoStatistics> ioStats,
      std::shared_ptr<filesystems::File::IoStats> fsStats,
      folly::Executor* executor,
      const io::ReaderOptions& readerOptions)
      : BufferedInput(
            std::move(readFile),
            readerOptions.memoryPool(),
            metricsLog,
            ioStats.get(),
            fsStats.get()),
        cache_(cache),
        fileNum_(std::move(fileNum)),
        tracker_(std::move(tracker)),
        groupId_(std::move(groupId)),
        ioStats_(std::move(ioStats)),
        fsStats_(std::move(fsStats)),
        executor_(executor),
        fileSize_(input_->getLength()),
        options_(readerOptions) {
    checkLoadQuantum();
  }

  CachedBufferedInput(
      std::shared_ptr<ReadFileInputStream> input,
      StringIdLease fileNum,
      cache::AsyncDataCache* cache,
      std::shared_ptr<cache::ScanTracker> tracker,
      StringIdLease groupId,
      std::shared_ptr<IoStatistics> ioStats,
      std::shared_ptr<filesystems::File::IoStats> fsStats,
      folly::Executor* executor,
      const io::ReaderOptions& readerOptions)
      : BufferedInput(std::move(input), readerOptions.memoryPool()),
        cache_(cache),
        fileNum_(std::move(fileNum)),
        tracker_(std::move(tracker)),
        groupId_(std::move(groupId)),
        ioStats_(std::move(ioStats)),
        fsStats_(std::move(fsStats)),
        executor_(executor),
        fileSize_(input_->getLength()),
        options_(readerOptions) {
    checkLoadQuantum();
  }

  ~CachedBufferedInput() override {
    for (auto& load : allCoalescedLoads_) {
      load->cancel();
    }
  }

  std::unique_ptr<SeekableInputStream> enqueue(
      velox::common::Region region,
      const StreamIdentifier* sid) override;

  bool supportSyncLoad() const override {
    return false;
  }

  void load(const LogType /*unused*/) override;

  bool isBuffered(uint64_t /*unused*/, uint64_t /*unused*/) const override;

  std::unique_ptr<SeekableInputStream>
  read(uint64_t offset, uint64_t length, LogType logType) const override;

  /// Schedules load of 'region' on 'executor_'. Fails silently if no memory or
  /// if shouldPreload() is false.
  bool prefetch(velox::common::Region region);

  bool shouldPreload(int32_t numPages = 0) override;

  bool shouldPrefetchStripes() const override {
    return true;
  }

  void setNumStripes(int32_t numStripes) override {
    auto stats = tracker_->fileGroupStats();
    if (stats) {
      stats->recordFile(fileNum_.id(), groupId_.id(), numStripes);
    }
  }

  virtual std::unique_ptr<BufferedInput> clone() const override {
    return std::make_unique<CachedBufferedInput>(
        input_,
        fileNum_,
        cache_,
        tracker_,
        groupId_,
        ioStats_,
        fsStats_,
        executor_,
        options_);
  }

  cache::AsyncDataCache* cache() const {
    return cache_;
  }

  /// Returns the CoalescedLoad that contains the correlated loads for 'stream'
  /// or nullptr if none. Returns nullptr on all but first call for 'stream'
  /// since the load is to be triggered by the first access.
  std::shared_ptr<cache::CoalescedLoad> coalescedLoad(
      const SeekableInputStream* stream);

  folly::Executor* executor() const override {
    return executor_;
  }

  uint64_t nextFetchSize() const override {
    VELOX_NYI();
  }

 private:
  template <bool kSsd>
  std::vector<int32_t> groupRequests(
      const std::vector<CacheRequest*>& requests,
      bool prefetch) const;

  // Makes a CoalescedLoad for 'requests' to be read together, coalescing IO is
  // appropriate. If 'prefetch' is set, schedules the CoalescedLoad on
  // 'executor_'. Links the CoalescedLoad to all CacheInputStreams that it
  // concerns.
  void readRegion(const std::vector<CacheRequest*>& requests, bool prefetch);

  // Read coalesced regions.  Regions are grouped together using `groupEnds'.
  // For example if there are 5 regions, 1 and 2 are coalesced together and 3,
  // 4, 5 are coalesced together, we will have {2, 5} in `groupEnds'.
  void readRegions(
      const std::vector<CacheRequest*>& requests,
      bool prefetch,
      const std::vector<int32_t>& groupEnds);

  template <bool kSsd>
  void makeLoads(std::vector<CacheRequest*> requests[2]);

  // We only support up to 8MB load quantum size on SSD and there is no need for
  // larger SSD read size performance wise.
  void checkLoadQuantum() {
    if (cache_->ssdCache() != nullptr) {
      VELOX_CHECK_LE(
          options_.loadQuantum(),
          1 << cache::SsdRun::kSizeBits,
          "Load quantum exceeded SSD cache entry size limit.");
    }
  }

  cache::AsyncDataCache* const cache_;
  const StringIdLease fileNum_;
  const std::shared_ptr<cache::ScanTracker> tracker_;
  const StringIdLease groupId_;
  const std::shared_ptr<IoStatistics> ioStats_;
  const std::shared_ptr<filesystems::File::IoStats> fsStats_;
  folly::Executor* const executor_;
  const uint64_t fileSize_;
  const io::ReaderOptions options_;

  // Regions that are candidates for loading.
  std::vector<CacheRequest> requests_;

  // Coalesced loads spanning multiple cache entries in one IO.
  folly::Synchronized<folly::F14FastMap<
      const SeekableInputStream*,
      std::shared_ptr<cache::CoalescedLoad>>>
      coalescedLoads_;

  // Distinct coalesced loads in 'coalescedLoads_'.
  std::vector<std::shared_ptr<cache::CoalescedLoad>> allCoalescedLoads_;
};

} // namespace facebook::velox::dwio::common
