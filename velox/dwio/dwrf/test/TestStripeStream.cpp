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

#include "velox/common/base/tests/GTestUtils.h"
#include "velox/dwio/common/encryption/TestProvider.h"
#include "velox/dwio/dwrf/reader/StripeStream.h"
#include "velox/dwio/dwrf/test/OrcTest.h"
#include "velox/dwio/dwrf/utils/ProtoUtils.h"
#include "velox/dwio/dwrf/writer/WriterBase.h"
#include "velox/type/Type.h"
#include "velox/type/fbhive/HiveTypeParser.h"

using namespace testing;
using namespace facebook::velox::dwio::common;
using namespace facebook::velox::dwio::common::encryption::test;
using namespace facebook::velox::dwrf;
using namespace facebook::velox::dwrf::encryption;
using namespace facebook::velox::memory;
using facebook::velox::RowType;
using facebook::velox::common::Region;
using facebook::velox::dwio::common::BufferedInput;
using facebook::velox::type::fbhive::HiveTypeParser;

class RecordingInputStream : public facebook::velox::InMemoryReadFile {
 public:
  RecordingInputStream() : InMemoryReadFile(std::string()) {}

  std::string_view pread(
      uint64_t offset,
      uint64_t length,
      void* buf,
      facebook::velox::filesystems::File::IoStats* stats =
          nullptr) const override {
    reads_.push_back({offset, length});
    return {static_cast<char*>(buf), length};
  }

  const std::vector<Region>& getReads() const {
    return reads_;
  }

 private:
  mutable std::vector<Region> reads_;
};

class TestProvider : public StrideIndexProvider {
 public:
  uint64_t getStrideIndex() const override {
    return 0;
  }
};

namespace {
void enqueueReads(
    BufferedInput& input,
    facebook::velox::dwrf::ReaderBase& readerBase,
    const StripeFooterWrapper& footer,
    const ColumnSelector& selector,
    uint64_t stripeStart,
    uint32_t stripeIndex) {
  auto& metadataCache = readerBase.metadataCache();
  uint64_t offset = stripeStart;
  uint64_t length = 0;
  if (footer.format() == DwrfFormat::kDwrf) {
    for (const auto& stream : footer.streamsDwrf()) {
      length = stream.length();
      // If index cache is available, there is no need to read it
      auto inMetaCache = metadataCache &&
          metadataCache->has(StripeCacheMode::INDEX, stripeIndex) &&
          static_cast<StreamKind>(stream.kind()) ==
              StreamKind::StreamKind_ROW_INDEX;
      if (length > 0 &&
          selector.shouldReadStream(stream.node(), stream.sequence()) &&
          !inMetaCache) {
        input.enqueue({offset, length});
      }
      offset += length;
    }
  } else {
    for (const auto& stream : footer.streamsOrc()) {
      EncodingKey ek{0, 0};

      length = stream.length();
      // If index cache is available, there is no need to read it
      auto inMetaCache = metadataCache &&
          metadataCache->has(StripeCacheMode::INDEX, stripeIndex) &&
          ek.forKind(stream.kind()).kind() ==
              StreamKind::StreamKindOrc_ROW_INDEX;
      if (length > 0 && selector.shouldReadStream(stream.column(), 0) &&
          !inMetaCache) {
        input.enqueue({offset, length});
      }
      offset += length;
    }
  }
}

StripeStreamsImpl createAndLoadStripeStreams(
    std::shared_ptr<StripeReadState> readState,
    const ColumnSelector& selector) {
  TestProvider indexProvider;
  StripeStreamsImpl streams{
      readState,
      &selector,
      nullptr,
      RowReaderOptions{},
      0,
      StripeStreamsImpl::kUnknownStripeRows,
      indexProvider,
      0};
  enqueueReads(
      *readState->stripeMetadata->stripeInput,
      *readState->readerBase,
      *readState->stripeMetadata->footer,
      selector,
      0,
      0);
  streams.loadReadPlan();
  return streams;
}

class StripeStreamTest : public testing::TestWithParam<DwrfFormat> {
 protected:
  static void SetUpTestCase() {
    MemoryManager::testingSetInstance(MemoryManager::Options{});
  }
  std::shared_ptr<MemoryPool> pool_{memoryManager()->addLeafPool()};
};

class StripeStreamFormatTypeTest : public testing::TestWithParam<DwrfFormat> {
 protected:
  static void SetUpTestCase() {
    MemoryManager::testingSetInstance(MemoryManager::Options{});
  }
  std::shared_ptr<MemoryPool> pool_{memoryManager()->addLeafPool()};
  DwrfFormat testParamDwrfFormat_ = GetParam();
};

INSTANTIATE_TEST_SUITE_P(
    StripeStreamFormatTypeTests,
    StripeStreamFormatTypeTest,
    ::testing::Values(DwrfFormat::kDwrf, DwrfFormat::kOrc));

} // namespace

TEST_P(StripeStreamFormatTypeTest, planReads) {
  google::protobuf::Arena arena;
  auto footer = google::protobuf::Arena::CreateMessage<proto::Footer>(&arena);
  footer->set_rowindexstride(100);
  auto type = HiveTypeParser().parse("struct<a:int,b:float>");
  ProtoUtils::writeType(*type, *footer);
  auto is = std::make_unique<RecordingInputStream>();
  auto isPtr = is.get();
  auto readerBase = std::make_shared<ReaderBase>(
      *pool_,
      std::make_unique<BufferedInput>(
          std::move(is),
          *pool_,
          MetricsLog::voidLog(),
          nullptr,
          nullptr,
          BufferedInput::kMaxMergeDistance,
          true),
      std::make_unique<PostScript>(proto::PostScript{}),
      footer,
      nullptr);
  ColumnSelector cs{readerBase->schema(), std::vector<uint64_t>{2}, true};

  TestDecrypterFactory factory;
  auto handler = DecryptionHandler::create(FooterWrapper(footer), &factory);
  std::unique_ptr<const StripeMetadata> stripeMetadata;

  if (testParamDwrfFormat_ == DwrfFormat::kDwrf) {
    auto stripeFooter = std::make_unique<proto::StripeFooter>();
    std::vector<std::tuple<uint64_t, StreamKind, uint64_t>> ss{
        std::make_tuple(1, StreamKind::StreamKind_ROW_INDEX, 100),
        std::make_tuple(2, StreamKind::StreamKind_ROW_INDEX, 100),
        std::make_tuple(1, StreamKind::StreamKind_PRESENT, 200),
        std::make_tuple(2, StreamKind::StreamKind_PRESENT, 200),
        std::make_tuple(1, StreamKind::StreamKind_DATA, 5000000),
        std::make_tuple(2, StreamKind::StreamKind_DATA, 1000000)};
    for (const auto& s : ss) {
      auto&& stream = stripeFooter->add_streams();
      stream->set_node(std::get<0>(s));
      stream->set_kind(static_cast<proto::Stream_Kind>(std::get<1>(s)));
      stream->set_length(std::get<2>(s));
    }

    stripeMetadata = std::make_unique<const StripeMetadata>(
        readerBase->bufferedInput().clone(),
        std::move(stripeFooter),
        std::move(handler),
        StripeInformationWrapper(
            static_cast<const proto::StripeInformation*>(nullptr)));
  } else {
    auto stripeFooterOrc = std::make_unique<proto::orc::StripeFooter>();
    std::vector<std::tuple<uint64_t, proto::orc::Stream_Kind, uint64_t>> ss{
        std::make_tuple(1, proto::orc::Stream_Kind_ROW_INDEX, 100),
        std::make_tuple(2, proto::orc::Stream_Kind_ROW_INDEX, 100),
        std::make_tuple(1, proto::orc::Stream_Kind_PRESENT, 200),
        std::make_tuple(2, proto::orc::Stream_Kind_PRESENT, 200),
        std::make_tuple(1, proto::orc::Stream_Kind_DATA, 5000000),
        std::make_tuple(2, proto::orc::Stream_Kind_DATA, 1000000)};
    for (const auto& s : ss) {
      auto&& stream = stripeFooterOrc->add_streams();
      stream->set_column(std::get<0>(s));
      stream->set_kind(std::get<1>(s));
      stream->set_length(std::get<2>(s));
    }

    stripeMetadata = std::make_unique<const StripeMetadata>(
        readerBase->bufferedInput().clone(),
        std::move(stripeFooterOrc),
        std::move(handler),
        StripeInformationWrapper(
            static_cast<const proto::StripeInformation*>(nullptr)));
  }

  auto stripeReadState =
      std::make_shared<StripeReadState>(readerBase, std::move(stripeMetadata));
  StripeReaderBase stripeReader{readerBase};
  auto streams = createAndLoadStripeStreams(stripeReadState, cs);
  auto const& actual = isPtr->getReads();
  ASSERT_FALSE(actual.empty());
  EXPECT_EQ(std::min(actual.cbegin(), actual.cend())->offset, 100);
  EXPECT_EQ(
      std::accumulate(
          actual.cbegin(),
          actual.cend(),
          0,
          [](uint64_t ac, const Region& r) { return ac + r.length; }),
      1000300);
}

TEST_F(StripeStreamTest, filterSequences) {
  google::protobuf::Arena arena;
  auto footer = google::protobuf::Arena::CreateMessage<proto::Footer>(&arena);
  footer->set_rowindexstride(100);
  auto type = HiveTypeParser().parse("struct<a:map<int,float>>");
  ProtoUtils::writeType(*type, *footer);
  auto is = std::make_unique<RecordingInputStream>();
  auto isPtr = is.get();
  auto readerBase = std::make_shared<ReaderBase>(
      *pool_,
      std::make_unique<BufferedInput>(std::move(is), *pool_),
      std::make_unique<PostScript>(proto::PostScript{}),
      footer,
      nullptr);

  // mock a filter that we only need one node and one sequence
  ColumnSelector cs{readerBase->schema(), std::vector<std::string>{"a#[1]"}};
  const auto& node = cs.getNode(1);
  auto seqFilter = std::make_shared<std::unordered_set<size_t>>();
  seqFilter->insert(1);
  node->setSequenceFilter(seqFilter);

  // mock the input stream data to verify our plan
  // only covered the filtered streams
  auto stripeFooter = std::make_unique<proto::StripeFooter>();
  std::vector<std::tuple<uint64_t, StreamKind, uint64_t, uint64_t>> ss{
      std::make_tuple(1, StreamKind::StreamKind_ROW_INDEX, 100, 0),
      std::make_tuple(2, StreamKind::StreamKind_ROW_INDEX, 100, 0),
      std::make_tuple(1, StreamKind::StreamKind_PRESENT, 200, 0),
      std::make_tuple(1, StreamKind::StreamKind_PRESENT, 200, 0),
      std::make_tuple(1, StreamKind::StreamKind_DATA, 5000000, 1),
      std::make_tuple(1, StreamKind::StreamKind_DATA, 3000000, 2),
      std::make_tuple(3, StreamKind::StreamKind_DATA, 1000000, 1)};

  for (const auto& s : ss) {
    auto&& stream = stripeFooter->add_streams();
    stream->set_node(std::get<0>(s));
    stream->set_kind(static_cast<proto::Stream_Kind>(std::get<1>(s)));
    stream->set_length(std::get<2>(s));
    stream->set_sequence(std::get<3>(s));
  }

  TestDecrypterFactory factory;
  auto handler = DecryptionHandler::create(FooterWrapper(footer), &factory);
  // filter by sequence 1
  std::vector<Region> expected{{600, 5000000}, {8000600, 1000000}};
  auto stripeMetadata = std::make_unique<const StripeMetadata>(
      readerBase->bufferedInput().clone(),
      std::move(stripeFooter),
      std::move(handler),
      StripeInformationWrapper(
          static_cast<const proto::StripeInformation*>(nullptr)));
  auto stripeReadState =
      std::make_shared<StripeReadState>(readerBase, std::move(stripeMetadata));
  StripeReaderBase stripeReader{readerBase};
  auto streams = createAndLoadStripeStreams(stripeReadState, cs);
  auto const& actual = isPtr->getReads();
  EXPECT_EQ(actual.size(), expected.size());
  for (uint64_t i = 0; i < actual.size(); ++i) {
    EXPECT_EQ(actual[i].offset, expected[i].offset);
    EXPECT_EQ(actual[i].length, expected[i].length);
  }
}

TEST_P(StripeStreamFormatTypeTest, zeroLength) {
  google::protobuf::Arena arena;
  auto footer = google::protobuf::Arena::CreateMessage<proto::Footer>(&arena);
  footer->set_rowindexstride(100);
  auto type = HiveTypeParser().parse("struct<a:int>");
  ProtoUtils::writeType(*type, *footer);
  proto::PostScript ps;
  ps.set_compressionblocksize(1024);
  ps.set_compression(proto::CompressionKind::ZSTD);
  auto is = std::make_unique<RecordingInputStream>();
  auto isPtr = is.get();
  auto readerBase = std::make_shared<ReaderBase>(
      *pool_,
      std::make_unique<BufferedInput>(std::move(is), *pool_),
      std::make_unique<PostScript>(std::move(ps)),
      footer,
      nullptr);

  TestDecrypterFactory factory;
  auto handler = DecryptionHandler::create(FooterWrapper(footer), &factory);
  std::unique_ptr<const StripeMetadata> stripeMetadata;

  std::vector<std::tuple<uint64_t, proto::Stream_Kind, uint64_t>>
      dwrfTestStreams = {
          std::make_tuple(0, proto::Stream_Kind_ROW_INDEX, 0),
          std::make_tuple(1, proto::Stream_Kind_ROW_INDEX, 0),
          std::make_tuple(1, proto::Stream_Kind_DATA, 0)};

  std::vector<std::tuple<uint64_t, proto::orc::Stream_Kind, uint64_t>>
      orcTestStreams{
          std::make_tuple(0, proto::orc::Stream_Kind_ROW_INDEX, 0),
          std::make_tuple(1, proto::orc::Stream_Kind_ROW_INDEX, 0),
          std::make_tuple(1, proto::orc::Stream_Kind_DATA, 0)};

  if (testParamDwrfFormat_ == DwrfFormat::kDwrf) {
    auto stripeFooter = std::make_unique<proto::StripeFooter>();

    for (const auto& s : dwrfTestStreams) {
      auto&& stream = stripeFooter->add_streams();
      stream->set_node(std::get<0>(s));
      stream->set_kind(std::get<1>(s));
      stream->set_length(std::get<2>(s));
    }

    stripeMetadata = std::make_unique<const StripeMetadata>(
        readerBase->bufferedInput().clone(),
        std::move(stripeFooter),
        std::move(handler),
        StripeInformationWrapper(
            static_cast<const proto::StripeInformation*>(nullptr)));
  } else {
    auto stripeFooterOrc = std::make_unique<proto::orc::StripeFooter>();
    for (const auto& s : orcTestStreams) {
      auto&& stream = stripeFooterOrc->add_streams();
      stream->set_column(std::get<0>(s));
      stream->set_kind(std::get<1>(s));
      stream->set_length(std::get<2>(s));
    }

    stripeMetadata = std::make_unique<const StripeMetadata>(
        readerBase->bufferedInput().clone(),
        std::move(stripeFooterOrc),
        std::move(handler),
        StripeInformationWrapper(
            static_cast<const proto::StripeInformation*>(nullptr)));
  }

  auto stripeReadState =
      std::make_shared<StripeReadState>(readerBase, std::move(stripeMetadata));
  StripeReaderBase stripeReader{readerBase};

  TestProvider indexProvider;
  ColumnSelector cs{std::dynamic_pointer_cast<const RowType>(type)};
  StripeStreamsImpl streams{
      stripeReadState,
      &cs,
      nullptr,
      RowReaderOptions{},
      0,
      StripeStreamsImpl::kUnknownStripeRows,
      indexProvider,
      0};
  streams.loadReadPlan();
  auto const& actual = isPtr->getReads();
  EXPECT_EQ(actual.size(), 0);

  if (testParamDwrfFormat_ == DwrfFormat::kDwrf) {
    for (const auto& s : dwrfTestStreams) {
      auto id = EncodingKey(std::get<0>(s)).forKind(std::get<1>(s));
      auto stream = streams.getStream(id, {}, true);
      EXPECT_NE(stream, nullptr);
      const void* buf = nullptr;
      int32_t size = 1;
      EXPECT_FALSE(stream->Next(&buf, &size));
      proto::RowIndex rowIndex;
      EXPECT_EQ(stream->positionSize(), 2);
    }
  } else {
    for (const auto& s : orcTestStreams) {
      auto id = EncodingKey(std::get<0>(s)).forKind(std::get<1>(s));
      auto stream = streams.getStream(id, {}, true);
      EXPECT_NE(stream, nullptr);
      const void* buf = nullptr;
      int32_t size = 1;
      EXPECT_FALSE(stream->Next(&buf, &size));
      proto::RowIndex rowIndex;
      EXPECT_EQ(stream->positionSize(), 2);
    }
  }
}

TEST_P(StripeStreamFormatTypeTest, planReadsIndex) {
  google::protobuf::Arena arena;

  // build ps
  proto::PostScript ps;
  ps.set_cachemode(proto::StripeCacheMode::INDEX);
  ps.set_compression(proto::CompressionKind::NONE);

  // build index
  proto::RowIndex index;
  index.add_entry()->add_positions(123);
  std::stringstream buffer;
  index.SerializeToOstream(&buffer);
  uint64_t length = buffer.tellp();
  index.SerializeToOstream(&buffer);

  // build footer
  auto footer = google::protobuf::Arena::CreateMessage<proto::Footer>(&arena);
  footer->set_rowindexstride(100);
  footer->add_stripecacheoffsets(0);
  footer->add_stripecacheoffsets(buffer.tellp());
  auto type = HiveTypeParser().parse("struct<a:int>");
  ProtoUtils::writeType(*type, *footer);

  // build cache
  std::string str(buffer.str());
  auto cacheBuffer = std::make_shared<DataBuffer<char>>(*pool_, str.size());
  memcpy(cacheBuffer->data(), str.data(), str.size());
  auto cache = std::make_unique<StripeMetadataCache>(
      StripeCacheMode::INDEX, FooterWrapper(footer), std::move(cacheBuffer));

  auto is = std::make_unique<RecordingInputStream>();
  auto isPtr = is.get();
  auto readerBase = std::make_shared<ReaderBase>(
      *pool_,
      std::make_unique<BufferedInput>(std::move(is), *pool_),
      std::make_unique<PostScript>(std::move(ps)),
      footer,
      std::move(cache));

  TestDecrypterFactory factory;
  auto handler = DecryptionHandler::create(FooterWrapper(footer), &factory);
  std::unique_ptr<const StripeMetadata> stripeMetadata;

  std::vector<std::tuple<uint64_t, proto::Stream_Kind, uint64_t>>
      dwrfTestStreams = {
          std::make_tuple(0, proto::Stream_Kind_ROW_INDEX, length),
          std::make_tuple(1, proto::Stream_Kind_ROW_INDEX, length),
          std::make_tuple(1, proto::Stream_Kind_PRESENT, 200),
          std::make_tuple(1, proto::Stream_Kind_DATA, 1000000)};

  std::vector<std::tuple<uint64_t, proto::orc::Stream_Kind, uint64_t>>
      orcTestStreams{
          std::make_tuple(0, proto::orc::Stream_Kind_ROW_INDEX, length),
          std::make_tuple(1, proto::orc::Stream_Kind_ROW_INDEX, length),
          std::make_tuple(1, proto::orc::Stream_Kind_PRESENT, 200),
          std::make_tuple(1, proto::orc::Stream_Kind_DATA, 1000000)};

  if (testParamDwrfFormat_ == DwrfFormat::kDwrf) {
    auto stripeFooter = std::make_unique<proto::StripeFooter>();

    for (const auto& s : dwrfTestStreams) {
      auto&& stream = stripeFooter->add_streams();
      stream->set_node(std::get<0>(s));
      stream->set_kind(std::get<1>(s));
      stream->set_length(std::get<2>(s));
    }

    stripeMetadata = std::make_unique<const StripeMetadata>(
        readerBase->bufferedInput().clone(),
        std::move(stripeFooter),
        std::move(handler),
        StripeInformationWrapper(
            static_cast<const proto::StripeInformation*>(nullptr)));
  } else {
    auto stripeFooterOrc = std::make_unique<proto::orc::StripeFooter>();
    for (const auto& s : orcTestStreams) {
      auto&& stream = stripeFooterOrc->add_streams();
      stream->set_column(std::get<0>(s));
      stream->set_kind(std::get<1>(s));
      stream->set_length(std::get<2>(s));
    }

    stripeMetadata = std::make_unique<const StripeMetadata>(
        readerBase->bufferedInput().clone(),
        std::move(stripeFooterOrc),
        std::move(handler),
        StripeInformationWrapper(
            static_cast<const proto::StripeInformation*>(nullptr)));
  }

  auto stripeReadState =
      std::make_shared<StripeReadState>(readerBase, std::move(stripeMetadata));
  StripeReaderBase stripeReader{readerBase};
  ColumnSelector cs{std::dynamic_pointer_cast<const RowType>(type)};
  auto streams = createAndLoadStripeStreams(stripeReadState, cs);
  auto const& actual = isPtr->getReads();
  ASSERT_FALSE(actual.empty());
  EXPECT_EQ(std::min(actual.cbegin(), actual.cend())->offset, length * 2);
  EXPECT_EQ(
      std::accumulate(
          actual.cbegin(),
          actual.cend(),
          0UL,
          [](uint64_t ac, const Region& r) { return ac + r.length; }),
      1000200);

  if (testParamDwrfFormat_ == DwrfFormat::kDwrf) {
    EXPECT_EQ(
        ProtoUtils::readProto<proto::RowIndex>(
            streams.getStream(
                EncodingKey(0).forKind(proto::Stream_Kind_ROW_INDEX), {}, true))
            ->entry(0)
            .positions(0),
        123);
    EXPECT_EQ(
        ProtoUtils::readProto<proto::RowIndex>(
            streams.getStream(
                EncodingKey(1).forKind(proto::Stream_Kind_ROW_INDEX), {}, true))
            ->entry(0)
            .positions(0),
        123);
  } else {
    EXPECT_EQ(
        ProtoUtils::readProto<proto::orc::RowIndex>(
            streams.getStream(
                EncodingKey(0).forKind(proto::orc::Stream_Kind_ROW_INDEX),
                {},
                true))
            ->entry(0)
            .positions(0),
        123);
    EXPECT_EQ(
        ProtoUtils::readProto<proto::orc::RowIndex>(
            streams.getStream(
                EncodingKey(1).forKind(proto::orc::Stream_Kind_ROW_INDEX),
                {},
                true))
            ->entry(0)
            .positions(0),
        123);
  }
}

void addEncryptionGroup(
    proto::Encryption& enc,
    const std::vector<uint32_t>& nodes) {
  auto group = enc.add_encryptiongroups();
  for (auto& n : nodes) {
    group->add_nodes(n);
    group->add_statistics();
  }
}

template <typename T>
void addNode(T& t, uint32_t node, uint32_t offset = 0) {
  auto enc = t.add_encoding();
  enc->set_kind(proto::ColumnEncoding_Kind_DIRECT);
  enc->set_node(node);
  enc->set_dictionarysize(node + 1);

  auto stream = t.add_streams();
  stream->set_kind(proto::Stream_Kind_DATA);
  stream->set_node(node);
  stream->set_length(node + 2);
  if (offset > 0) {
    stream->set_offset(offset);
  }
}

TEST_F(StripeStreamTest, readEncryptedStreams) {
  auto pool = memoryManager()->addRootPool("readEncryptedStreams");
  google::protobuf::Arena arena;
  proto::PostScript ps;
  ps.set_compression(proto::CompressionKind::ZSTD);
  ps.set_compressionblocksize(256 * 1024);
  auto footer = google::protobuf::Arena::CreateMessage<proto::Footer>(&arena);
  // a: not encrypted, projected
  // encryption group 1: b, c. projected b.
  // group 2: d. projected d.
  // group 3: e. not projected
  auto type = HiveTypeParser().parse("struct<a:int,b:int,c:int,d:int,e:int>");
  ProtoUtils::writeType(*type, *footer);

  auto enc = footer->mutable_encryption();
  enc->set_keyprovider(proto::Encryption_KeyProvider_UNKNOWN);
  addEncryptionGroup(*enc, {2, 3});
  addEncryptionGroup(*enc, {4});
  addEncryptionGroup(*enc, {5});

  auto stripe = footer->add_stripes();
  for (auto i = 0; i < 3; ++i) {
    *stripe->add_keymetadata() = folly::to<std::string>("key", i);
  }
  TestDecrypterFactory factory;
  auto handler = DecryptionHandler::create(FooterWrapper(footer), &factory);
  TestEncrypter encrypter;

  auto sinkPool = pool->addLeafChild("sink");
  ProtoWriter pw{pool, *sinkPool};
  auto stripeFooter = std::make_unique<proto::StripeFooter>();
  addNode(*stripeFooter, 1);
  proto::StripeEncryptionGroup group1;
  addNode(group1, 2, 100);
  addNode(group1, 3);
  encrypter.setKey("key0");
  pw.writeProto(*stripeFooter->add_encryptiongroups(), group1, encrypter);
  proto::StripeEncryptionGroup group2;
  addNode(group2, 4, 200);
  encrypter.setKey("key1");
  pw.writeProto(*stripeFooter->add_encryptiongroups(), group2, encrypter);
  // add empty string to group3, so decoding will fail if read
  *stripeFooter->add_encryptiongroups() = "";

  auto readerPool = pool->addLeafChild("reader");
  auto readerBase = std::make_shared<ReaderBase>(
      *readerPool,
      std::make_unique<BufferedInput>(
          std::make_shared<facebook::velox::InMemoryReadFile>(std::string()),
          *readerPool),
      std::make_unique<PostScript>(std::move(ps)),
      footer,
      nullptr,
      std::move(handler));
  auto stripeMetadata = std::make_unique<const StripeMetadata>(
      &readerBase->bufferedInput(),
      std::move(stripeFooter),
      DecryptionHandler::create(FooterWrapper(footer), &factory),
      StripeInformationWrapper(
          static_cast<const proto::StripeInformation*>(nullptr)));
  auto stripeReadState =
      std::make_shared<StripeReadState>(readerBase, std::move(stripeMetadata));
  StripeReaderBase stripeReader{readerBase};
  ColumnSelector selector{readerBase->schema(), {1, 2, 4}, true};
  TestProvider provider;
  StripeStreamsImpl streams{
      stripeReadState,
      &selector,
      nullptr,
      RowReaderOptions{},
      0,
      StripeStreamsImpl::kUnknownStripeRows,
      provider,
      0};

  // make sure projected columns exist
  std::unordered_set<uint32_t> existed{1, 2, 4};
  for (uint32_t node = 1; node < 6; ++node) {
    EncodingKey ek{node};
    auto stream = streams.getStream(
        DwrfStreamIdentifier{node, 0, 0, StreamKind::StreamKind_DATA},
        {},
        false);
    if (existed.count(node)) {
      ASSERT_EQ(streams.getEncoding(ek).dictionarysize(), node + 1);
      ASSERT_NE(stream, nullptr);
    } else {
      VELOX_ASSERT_THROW(streams.getEncoding(ek), "encoding not found");
      ASSERT_EQ(stream, nullptr);
    }
  }
}

TEST_F(StripeStreamTest, schemaMismatch) {
  auto pool = memoryManager()->addRootPool("schemaMismatch");
  google::protobuf::Arena arena;
  proto::PostScript ps;
  ps.set_compression(proto::CompressionKind::ZSTD);
  ps.set_compressionblocksize(256 * 1024);
  auto footer = google::protobuf::Arena::CreateMessage<proto::Footer>(&arena);
  // a: not encrypted, has schema change
  // b: encrypted
  // c: not encrypted
  auto type = HiveTypeParser().parse("struct<a:struct<a:int>,b:int,c:int>");
  ProtoUtils::writeType(*type, *footer);

  auto enc = footer->mutable_encryption();
  enc->set_keyprovider(proto::Encryption_KeyProvider_UNKNOWN);
  addEncryptionGroup(*enc, {3});

  auto stripe = footer->add_stripes();
  *stripe->add_keymetadata() = "key";
  TestDecrypterFactory factory;
  auto handler = DecryptionHandler::create(FooterWrapper(footer), &factory);
  TestEncrypter encrypter;

  auto sinkPool = pool->addLeafChild("sink");
  ProtoWriter pw{pool, *sinkPool};
  auto stripeFooter = std::make_unique<proto::StripeFooter>();
  addNode(*stripeFooter, 1);
  addNode(*stripeFooter, 2);
  addNode(*stripeFooter, 4);
  proto::StripeEncryptionGroup group;
  addNode(group, 3, 100);
  encrypter.setKey("key");
  pw.writeProto(*stripeFooter->add_encryptiongroups(), group, encrypter);

  auto readerPool = pool->addLeafChild("reader");
  auto readerBase = std::make_shared<ReaderBase>(
      *readerPool,
      std::make_unique<BufferedInput>(
          std::make_shared<facebook::velox::InMemoryReadFile>(std::string()),
          *pool_),
      std::make_unique<PostScript>(std::move(ps)),
      footer,
      nullptr,
      std::move(handler));
  auto stripeMetadata = std::make_unique<const StripeMetadata>(
      &readerBase->bufferedInput(),
      std::move(stripeFooter),
      DecryptionHandler::create(FooterWrapper(footer), &factory),
      StripeInformationWrapper(
          static_cast<const proto::StripeInformation*>(nullptr)));
  auto stripeReadState =
      std::make_shared<StripeReadState>(readerBase, std::move(stripeMetadata));
  StripeReaderBase stripeReader{readerBase};
  // now, we read the file as if schema has changed
  auto schema =
      HiveTypeParser().parse("struct<a:struct<a1:int,a2:int>,b:int,c:int>");
  // only project b and c. Node id of b and c in the new schema is 4, 5
  ColumnSelector selector{
      std::dynamic_pointer_cast<const RowType>(schema), {4, 5}, true};
  TestProvider provider;
  StripeStreamsImpl streams{
      stripeReadState,
      &selector,
      nullptr,
      RowReaderOptions{},
      0,
      StripeStreamsImpl::kUnknownStripeRows,
      provider,
      0};

  // make sure all columns exist. Node id of b and c in the file is 3, 4
  for (uint32_t node = 3; node < 4; ++node) {
    EncodingKey ek{node};
    auto stream = streams.getStream(
        DwrfStreamIdentifier{node, 0, 0, StreamKind::StreamKind_DATA},
        {},
        false);
    ASSERT_EQ(streams.getEncoding(ek).dictionarysize(), node + 1);
    ASSERT_NE(stream, nullptr);
  }
}

namespace {
// A class to allow testing StripeStreamsBase functionality with minimally
// needed methods implemented.
class TestStripeStreams : public StripeStreamsBase {
 public:
  explicit TestStripeStreams(MemoryPool* pool) : StripeStreamsBase{pool} {}

  const proto::ColumnEncoding& getEncoding(
      const EncodingKey& ek) const override {
    return *getEncodingProxy(ek.node(), ek.sequence());
  }

  const proto::orc::ColumnEncoding& getEncodingOrc(
      const EncodingKey& ek) const override {
    return *getEncodingOrcProxy(ek.node(), ek.sequence());
  }

  std::unique_ptr<SeekableInputStream> getStream(
      const DwrfStreamIdentifier& si,
      std::string_view /* label */,
      bool throwIfNotFound) const override {
    return std::unique_ptr<SeekableInputStream>(getStreamProxy(
        si.encodingKey().node(),
        si.encodingKey().sequence(),
        static_cast<proto::Stream_Kind>(si.kind()),
        throwIfNotFound));
  }

  const facebook::velox::dwio::common::ColumnSelector& getColumnSelector()
      const override {
    VELOX_UNSUPPORTED();
  }

  const facebook::velox::tz::TimeZone* sessionTimezone() const override {
    VELOX_UNSUPPORTED();
  }

  bool adjustTimestampToTimezone() const override {
    return false;
  }

  const facebook::velox::dwio::common::RowReaderOptions& rowReaderOptions()
      const override {
    VELOX_UNSUPPORTED();
  }

  const StrideIndexProvider& getStrideIndexProvider() const override {
    VELOX_UNSUPPORTED();
  }

  bool getUseVInts(const DwrfStreamIdentifier& /* unused */) const override {
    return true; // current tests all expect results from using vints
  }

  int64_t stripeRows() const override {
    VELOX_UNSUPPORTED();
  }

  uint32_t rowsPerRowGroup() const override {
    VELOX_UNSUPPORTED();
  }

  MOCK_CONST_METHOD2(
      getEncodingProxy,
      proto::ColumnEncoding*(uint32_t, uint32_t));
  MOCK_CONST_METHOD2(
      getEncodingOrcProxy,
      proto::orc::ColumnEncoding*(uint32_t, uint32_t));
  MOCK_CONST_METHOD2(
      visitStreamsOfNode,
      uint32_t(uint32_t, std::function<void(const StreamInformation&)>));
  MOCK_CONST_METHOD4(
      getStreamProxy,
      SeekableInputStream*(uint32_t, uint32_t, proto::Stream_Kind, bool));

  std::shared_ptr<MemoryPool> pool_;
};

proto::ColumnEncoding genColumnEncoding(
    uint32_t node,
    uint32_t sequence,
    const proto::ColumnEncoding_Kind& kind,
    size_t dictionarySize) {
  proto::ColumnEncoding encoding;
  encoding.set_node(node);
  encoding.set_sequence(sequence);
  encoding.set_kind(kind);
  encoding.set_dictionarysize(dictionarySize);
  return encoding;
}
} // namespace

TEST_F(StripeStreamTest, shareDictionary) {
  TestStripeStreams ss(pool_.get());

  auto nonSharedDictionaryEncoding =
      genColumnEncoding(1, 0, proto::ColumnEncoding_Kind_DICTIONARY, 100);
  EXPECT_CALL(ss, getEncodingProxy(1, 0))
      .WillOnce(Return(&nonSharedDictionaryEncoding));
  char nonSharedDictBuffer[1024];
  size_t nonSharedDictBufferSize = writeRange(nonSharedDictBuffer, 0, 100);
  EXPECT_CALL(ss, getStreamProxy(1, 0, proto::Stream_Kind_DICTIONARY_DATA, _))
      .WillOnce(InvokeWithoutArgs([&]() {
        return new SeekableArrayInputStream(
            nonSharedDictBuffer, nonSharedDictBufferSize);
      }));
  auto sharedDictionaryEncoding2_2 =
      genColumnEncoding(2, 2, proto::ColumnEncoding_Kind_DICTIONARY, 100);
  EXPECT_CALL(ss, getEncodingProxy(2, 2))
      .WillOnce(Return(&sharedDictionaryEncoding2_2));
  auto sharedDictionaryEncoding2_3 =
      genColumnEncoding(2, 3, proto::ColumnEncoding_Kind_DICTIONARY, 100);
  EXPECT_CALL(ss, getEncodingProxy(2, 3))
      .WillOnce(Return(&sharedDictionaryEncoding2_3));
  auto sharedDictionaryEncoding2_5 =
      genColumnEncoding(2, 5, proto::ColumnEncoding_Kind_DICTIONARY, 100);
  EXPECT_CALL(ss, getEncodingProxy(2, 5))
      .WillOnce(Return(&sharedDictionaryEncoding2_5));
  auto sharedDictionaryEncoding2_8 =
      genColumnEncoding(2, 8, proto::ColumnEncoding_Kind_DICTIONARY, 100);
  EXPECT_CALL(ss, getEncodingProxy(2, 8))
      .WillOnce(Return(&sharedDictionaryEncoding2_8));
  auto sharedDictionaryEncoding2_13 =
      genColumnEncoding(2, 13, proto::ColumnEncoding_Kind_DICTIONARY, 100);
  EXPECT_CALL(ss, getEncodingProxy(2, 13))
      .WillOnce(Return(&sharedDictionaryEncoding2_13));
  auto sharedDictionaryEncoding2_21 =
      genColumnEncoding(2, 21, proto::ColumnEncoding_Kind_DICTIONARY, 100);
  EXPECT_CALL(ss, getEncodingProxy(2, 21))
      .WillOnce(Return(&sharedDictionaryEncoding2_21));
  char sharedDictBuffer[2048];
  size_t sharedDictBufferSize = writeRange(sharedDictBuffer, 100, 200);
  EXPECT_CALL(ss, getStreamProxy(2, 0, proto::Stream_Kind_DICTIONARY_DATA, _))
      .WillRepeatedly(InvokeWithoutArgs([&]() {
        return new SeekableArrayInputStream(
            sharedDictBuffer, sharedDictBufferSize);
      }));
  EXPECT_CALL(
      ss, getStreamProxy(2, Not(0), proto::Stream_Kind_DICTIONARY_DATA, _))
      .WillRepeatedly(Return(nullptr));

  facebook::velox::memory::AllocationPool allocPool(pool_.get());
  StreamLabels labels(allocPool);
  std::vector<std::function<facebook::velox::BufferPtr()>> dictInits{};
  dictInits.push_back(
      ss.getIntDictionaryInitializerForNode(EncodingKey{1, 0}, 8, labels));
  dictInits.push_back(
      ss.getIntDictionaryInitializerForNode(EncodingKey{2, 2}, 16, labels));
  dictInits.push_back(
      ss.getIntDictionaryInitializerForNode(EncodingKey{2, 3}, 4, labels));
  dictInits.push_back(
      ss.getIntDictionaryInitializerForNode(EncodingKey{2, 5}, 16, labels));
  dictInits.push_back(
      ss.getIntDictionaryInitializerForNode(EncodingKey{2, 8}, 8, labels));
  dictInits.push_back(
      ss.getIntDictionaryInitializerForNode(EncodingKey{2, 13}, 4, labels));
  dictInits.push_back(
      ss.getIntDictionaryInitializerForNode(EncodingKey{2, 21}, 16, labels));

  auto dictCache = ss.getStripeDictionaryCache();
  // Maybe verify range is useful here.
  EXPECT_NO_THROW(dictCache->getIntDictionary({1, 0}));
  EXPECT_NO_THROW(dictCache->getIntDictionary({2, 0}));
  EXPECT_ANY_THROW(dictCache->getIntDictionary({2, 2}));
  EXPECT_ANY_THROW(dictCache->getIntDictionary({2, 3}));
  EXPECT_ANY_THROW(dictCache->getIntDictionary({2, 5}));
  EXPECT_ANY_THROW(dictCache->getIntDictionary({2, 8}));
  EXPECT_ANY_THROW(dictCache->getIntDictionary({2, 13}));
  EXPECT_ANY_THROW(dictCache->getIntDictionary({2, 21}));

  for (auto& dictInit : dictInits) {
    EXPECT_NO_THROW(dictInit());
  }
}
