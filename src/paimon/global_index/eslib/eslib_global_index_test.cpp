/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "arrow/c/bridge.h"
#include "arrow/type.h"
#include "gtest/gtest.h"
#include "paimon/common/global_index/offset_global_index_reader.h"
#include "paimon/core/global_index/global_index_scan_impl.h"
#include "paimon/core/manifest/index_manifest_file.h"
#include "paimon/data/timestamp.h"
#include "paimon/format/file_format_factory.h"
#include "paimon/fs/local/local_file_system.h"
#include "paimon/global_index/global_indexer.h"
#include "paimon/global_index/global_indexer_factory.h"
#include "paimon/memory/bytes.h"
#include "paimon/predicate/predicate_builder.h"
#include "paimon/testing/utils/testharness.h"

namespace paimon::test {
namespace {
class FixtureReader final : public GlobalIndexFileReader {
 public:
    Result<std::unique_ptr<InputStream>> GetInputStream(const std::string& path) const override {
        return LocalFileSystem().Open(path);
    }
};
std::string Load(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
Result<std::set<int64_t>> Rows(const std::shared_ptr<GlobalIndexResult>& result) {
    if (!result) return Status::Invalid("Missing index result");
    PAIMON_ASSIGN_OR_RAISE(std::unique_ptr<GlobalIndexResult::Iterator> iter,
                           result->CreateIterator());
    std::set<int64_t> rows;
    while (iter->HasNext()) rows.insert(iter->Next());
    return rows;
}
std::vector<float> Vector(int row, bool normalized = false) {
    std::vector<float> value(33);
    if (normalized) {
        value[0] = std::cos(row * 0.01);
        value[1] = std::sin(row * 0.01);
    } else {
        for (size_t i = 0; i < value.size(); ++i) value[i] = row * 0.25f + i * 0.01f;
    }
    return value;
}
std::shared_ptr<VectorSearch> Search(const std::vector<float>& vector, int limit = 1) {
    return std::make_shared<VectorSearch>("embedding", limit, vector, nullptr, nullptr,
                                          std::nullopt, std::map<std::string, std::string>());
}
}  // namespace
class ESLibIndexTest : public ::testing::Test {
 protected:
    void SetUp() override {
        const char* fixtures = std::getenv("PAIMON_ESLIB_FIXTURE_DIR");
        const char* library = std::getenv("PAIMON_ESLIB_NATIVE_LIBRARY");
        const char* commit = std::getenv("PAIMON_ESLIB_NATIVE_COMMIT");
        const char* sha = std::getenv("PAIMON_ESLIB_NATIVE_SHA256");
        if (!fixtures || !library || !commit || !sha)
            GTEST_SKIP()
                << "Generate Java native fixtures and set PAIMON_ESLIB_* runtime variables";
        root_ = fixtures;
        options_ = {{"global-index.es-index.native.library-path", library},
                    {"global-index.es-index.native.commit", commit},
                    {"global-index.es-index.native.sha256", sha}};
        pool_ = GetDefaultPool();
    }
    Result<std::shared_ptr<GlobalIndexReader>> Open(
        const std::string& fixture, const std::string& field = "embedding",
        std::shared_ptr<arrow::DataType> type = arrow::fixed_size_list(arrow::float32(), 33),
        const std::map<std::string, std::string>* options = nullptr,
        const GlobalIndexIOMeta* supplied = nullptr) {
        PAIMON_ASSIGN_OR_RAISE(
            std::unique_ptr<GlobalIndexer> indexer,
            GlobalIndexerFactory::Get("es-index", options ? *options : options_));
        if (!indexer) return Status::Invalid("ESLib factory not linked");
        std::string path = root_ + "/" + fixture + "/archive.bin";
        std::error_code error;
        auto size = std::filesystem::file_size(path, error);
        if (error) return Status::IOError("Missing Java fixture: ", path);
        auto metadata =
            std::make_shared<Bytes>(Load(root_ + "/" + fixture + "/metadata.bin"), pool_.get());
        GlobalIndexIOMeta meta(path, size, metadata);
        ArrowSchema c_schema;
        auto exported = arrow::ExportSchema(*arrow::schema({arrow::field(field, type)}), &c_schema);
        if (!exported.ok()) return Status::Invalid(exported.ToString());
        auto result = indexer->CreateReader(&c_schema, std::make_shared<FixtureReader>(),
                                            {supplied ? *supplied : meta}, pool_);
        if (c_schema.release) c_schema.release(&c_schema);
        return result;
    }
    std::string root_;
    std::map<std::string, std::string> options_;
    std::shared_ptr<MemoryPool> pool_;
};
TEST(ESLibFactoryTest, RegisteredAndWriteUnsupported) {
    ASSERT_OK_AND_ASSIGN(std::unique_ptr<GlobalIndexer> indexer,
                         GlobalIndexerFactory::Get("es-index", {}));
    ASSERT_TRUE(indexer);
    ASSERT_NOK_WITH_MSG(indexer->CreateWriter("id", nullptr, nullptr, nullptr),
                        "Java native writer");
    ASSERT_NOK_WITH_MSG(indexer->CreateReader(nullptr, nullptr, {}, nullptr), "one schema");
}
TEST_F(ESLibIndexTest, ReadsThreeJavaVectorAlgorithmsWithSparseRows) {
    for (const char* algorithm : {"hnsw", "int8_hnsw", "diskbbq"}) {
        SCOPED_TRACE(algorithm);
        ASSERT_OK_AND_ASSIGN(std::shared_ptr<GlobalIndexReader> reader, Open(algorithm));
        ASSERT_TRUE(reader->IsThreadSafe());
        ASSERT_EQ(reader->GetIndexType(), "es-index");
        ASSERT_OK_AND_ASSIGN(std::shared_ptr<ScoredGlobalIndexResult> result,
                             reader->VisitVectorSearch(Search(Vector(37))));
        ASSERT_OK_AND_ASSIGN(std::set<int64_t> rows, Rows(result));
        ASSERT_EQ(rows, std::set<int64_t>({37}));
        ASSERT_OK_AND_ASSIGN(std::unique_ptr<ScoredGlobalIndexResult::ScoredIterator> iter,
                             result->CreateScoredIterator());
        ASSERT_NEAR(iter->NextWithScore().second, 1.0f, 1e-5);
        ASSERT_OK_AND_ASSIGN(std::shared_ptr<GlobalIndexResult> nulls, reader->VisitIsNull());
        ASSERT_OK_AND_ASSIGN(rows, Rows(nulls));
        std::set<int64_t> expected;
        for (int32_t row = 0; row < 128; row += 7) expected.insert(row);
        ASSERT_EQ(rows, expected);
    }
}
TEST_F(ESLibIndexTest, PreservesLongPrecisionAndNullSemantics) {
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<GlobalIndexReader> reader,
                         Open("hnsw", "id", arrow::int64()));
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<GlobalIndexResult> result,
                         reader->VisitEqual(Literal(static_cast<int64_t>(9007199254741030LL))));
    ASSERT_OK_AND_ASSIGN(std::set<int64_t> rows, Rows(result));
    ASSERT_EQ(rows, std::set<int64_t>({37}));
    ASSERT_OK_AND_ASSIGN(
        result, reader->VisitGreaterThan(Literal(static_cast<int64_t>(9007199254741118LL))));
    ASSERT_OK_AND_ASSIGN(rows, Rows(result));
    ASSERT_EQ(rows, std::set<int64_t>({126, 127}));
    ASSERT_OK_AND_ASSIGN(result,
                         reader->VisitNotEqual(Literal(static_cast<int64_t>(9007199254741030LL))));
    ASSERT_OK_AND_ASSIGN(rows, Rows(result));
    ASSERT_EQ(rows.size(), 115u);
    ASSERT_EQ(rows.count(37), 0u);
    ASSERT_EQ(rows.count(0), 0u);
    ASSERT_OK_AND_ASSIGN(result,
                         reader->VisitNotIn({Literal(static_cast<int64_t>(9007199254741030LL)),
                                             Literal(FieldType::BIGINT)}));
    ASSERT_OK_AND_ASSIGN(rows, Rows(result));
    ASSERT_TRUE(rows.empty());
    ASSERT_OK_AND_ASSIGN(result,
                         reader->VisitIn({Literal(static_cast<int64_t>(9007199254741030LL)),
                                          Literal(static_cast<int64_t>(9007199254741031LL))}));
    ASSERT_OK_AND_ASSIGN(rows, Rows(result));
    ASSERT_EQ(rows, std::set<int64_t>({37, 38}));
}
TEST_F(ESLibIndexTest, TextExactCompanionAndLiteralWildcards) {
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<GlobalIndexReader> reader,
                         Open("hnsw", "title", arrow::utf8()));
    Literal title(FieldType::STRING, "document 37 odd", 15);
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<GlobalIndexResult> result, reader->VisitEqual(title));
    ASSERT_OK_AND_ASSIGN(std::set<int64_t> rows, Rows(result));
    ASSERT_EQ(rows, std::set<int64_t>({37}));
    auto search =
        std::make_shared<FullTextSearch>("title", "{\"match\":{\"query\":\"even\"}}", 128);
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<ScoredGlobalIndexResult> scored,
                         reader->VisitFullTextSearch(search));
    ASSERT_OK_AND_ASSIGN(rows, Rows(scored));
    ASSERT_EQ(rows.size(), 64u);
    ASSERT_OK_AND_ASSIGN(reader, Open("hnsw", "tag", arrow::utf8()));
    Literal suffix(FieldType::STRING, "_37*?", 5);
    ASSERT_OK_AND_ASSIGN(result, reader->VisitEndsWith(suffix));
    ASSERT_OK_AND_ASSIGN(rows, Rows(result));
    ASSERT_EQ(rows, std::set<int64_t>({37}));
}
TEST_F(ESLibIndexTest, FullTextPrefilterAndPhrase) {
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<GlobalIndexReader> reader,
                         Open("hnsw", "title", arrow::utf8()));
    RoaringBitmap64 include;
    include.Add(36);
    include.Add(37);
    include.Add(10000);
    auto search =
        std::make_shared<FullTextSearch>("title", "{\"match\":{\"query\":\"even\"}}", 10, include);
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<ScoredGlobalIndexResult> result,
                         reader->VisitFullTextSearch(search));
    ASSERT_OK_AND_ASSIGN(std::set<int64_t> rows, Rows(result));
    ASSERT_EQ(rows, std::set<int64_t>({36}));
    search->query = "{\"match_phrase\":{\"query\":\"document 37 odd\"}}";
    search->include_row_ids = std::nullopt;
    ASSERT_OK_AND_ASSIGN(result, reader->VisitFullTextSearch(search));
    ASSERT_OK_AND_ASSIGN(rows, Rows(result));
    ASSERT_EQ(rows, std::set<int64_t>({37}));
}
TEST_F(ESLibIndexTest, GlobalOffsetIsAppliedOnceToResultsAndPrefilters) {
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<GlobalIndexReader> local, Open("hnsw"));
    auto reader = std::make_shared<OffsetGlobalIndexReader>(std::move(local), 9007199254740993LL);
    auto search = Search(Vector(37));
    search->pre_filter = [](int64_t row) { return row == 9007199254741031LL; };
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<ScoredGlobalIndexResult> result,
                         reader->VisitVectorSearch(search));
    ASSERT_OK_AND_ASSIGN(std::set<int64_t> rows, Rows(result));
    ASSERT_EQ(rows, std::set<int64_t>({9007199254741031LL}));
    ASSERT_OK_AND_ASSIGN(local, Open("hnsw", "title", arrow::utf8()));
    reader = std::make_shared<OffsetGlobalIndexReader>(std::move(local), 9007199254740993LL);
    RoaringBitmap64 include;
    include.Add(9007199254741029LL);
    include.Add(2);
    auto text =
        std::make_shared<FullTextSearch>("title", "{\"match\":{\"query\":\"even\"}}", 10, include);
    ASSERT_OK_AND_ASSIGN(result, reader->VisitFullTextSearch(text));
    ASSERT_OK_AND_ASSIGN(rows, Rows(result));
    ASSERT_EQ(rows, std::set<int64_t>({9007199254741029LL}));
}
TEST_F(ESLibIndexTest, VectorPredicateAndNormalizedMetricScores) {
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<GlobalIndexReader> reader, Open("hnsw"));
    auto search = Search(Vector(37));
    search->predicate = PredicateBuilder::Equal(2, "id", FieldType::BIGINT,
                                                Literal(static_cast<int64_t>(9007199254741031LL)));
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<ScoredGlobalIndexResult> result,
                         reader->VisitVectorSearch(search));
    ASSERT_OK_AND_ASSIGN(std::set<int64_t> rows, Rows(result));
    ASSERT_EQ(rows, std::set<int64_t>({38}));
    for (const char* metric : {"cosine", "dot_product"}) {
        ASSERT_OK_AND_ASSIGN(reader, Open(metric));
        ASSERT_OK_AND_ASSIGN(result, reader->VisitVectorSearch(Search(Vector(37, true))));
        ASSERT_OK_AND_ASSIGN(rows, Rows(result));
        ASSERT_EQ(rows, std::set<int64_t>({37}));
        ASSERT_OK_AND_ASSIGN(std::unique_ptr<ScoredGlobalIndexResult::ScoredIterator> iter,
                             result->CreateScoredIterator());
        ASSERT_NEAR(iter->NextWithScore().second, 1.0f, 1e-5);
    }
}
TEST_F(ESLibIndexTest, ReadsJavaIkAnalyzerIndex) {
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<GlobalIndexReader> reader,
                         Open("ik", "title", arrow::utf8()));
    auto search = std::make_shared<FullTextSearch>(
        "title", "{\"match\":{\"query\":\"中华人民共和国\"}}", 128);
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<ScoredGlobalIndexResult> result,
                         reader->VisitFullTextSearch(search));
    ASSERT_OK_AND_ASSIGN(std::set<int64_t> rows, Rows(result));
    ASSERT_EQ(rows.size(), 127u);
}
TEST_F(ESLibIndexTest, RejectsSchemaRuntimeMetadataAndArchiveMismatches) {
    ASSERT_NOK_WITH_MSG(Open("hnsw", "absent", arrow::int64()), "absent");
    ASSERT_NOK_WITH_MSG(Open("hnsw", "id", arrow::float32()), "schema");
    auto wrong = options_;
    wrong["global-index.es-index.native.sha256"] = std::string(64, '0');
    ASSERT_NOK_WITH_MSG(
        Open("hnsw", "embedding", arrow::fixed_size_list(arrow::float32(), 33), &wrong),
        "must match");
    std::string path = root_ + "/hnsw/archive.bin";
    auto size = std::filesystem::file_size(path);
    std::string data = Load(root_ + "/hnsw/metadata.bin");
    data[7] = 2;
    GlobalIndexIOMeta meta(path, size, std::make_shared<Bytes>(data, pool_.get()));
    ASSERT_NOK_WITH_MSG(Open("hnsw", "id", arrow::int64(), nullptr, &meta), "ESM1 v3");
    data = Load(root_ + "/hnsw/metadata.bin");
    data += 'x';
    meta.metadata = std::make_shared<Bytes>(data, pool_.get());
    ASSERT_NOK_WITH_MSG(Open("hnsw", "id", arrow::int64(), nullptr, &meta), "Trailing ESM1");
    meta.metadata = std::make_shared<Bytes>(Load(root_ + "/hnsw/metadata.bin"), pool_.get());
    meta.file_size = size - 1;
    ASSERT_NOK(Open("hnsw", "id", arrow::int64(), nullptr, &meta));
}
TEST_F(ESLibIndexTest, RejectsInvalidQueriesAndMaintainsConcurrentReaders) {
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<GlobalIndexReader> reader, Open("hnsw"));
    auto invalid = Search({1, 2});
    ASSERT_NOK_WITH_MSG(reader->VisitVectorSearch(invalid), "dimension");
    invalid = Search(Vector(37));
    invalid->distance_type = VectorSearch::DistanceType::COSINE;
    ASSERT_NOK_WITH_MSG(reader->VisitVectorSearch(invalid), "distance");
    std::vector<std::future<bool>> futures;
    for (int32_t i = 0; i < 4; ++i) {
        futures.push_back(std::async(std::launch::async, [reader]() {
            auto result = reader->VisitVectorSearch(Search(Vector(37)));
            if (!result.ok()) return false;
            auto rows = Rows(result.value());
            return rows.ok() && rows.value() == std::set<int64_t>{37};
        }));
    }
    for (auto& future : futures) ASSERT_TRUE(future.get());
    ASSERT_OK_AND_ASSIGN(reader, Open("hnsw", "title", arrow::utf8()));
    ASSERT_NOK(reader->VisitFullTextSearch(std::make_shared<FullTextSearch>("title", "{", 1)));
    ASSERT_NOK(reader->VisitFullTextSearch(std::make_shared<FullTextSearch>(
        "title", "{\"match\":{\"query\":\"even\",\"fuzziness\":1}}", 1)));
}

TEST_F(ESLibIndexTest, DiscoversEveryJavaIndexedFieldThroughSnapshotManifest) {
    auto directory = UniqueTestDirectory::Create();
    ASSERT_TRUE(directory);
    auto options_map = options_;
    options_map[Options::FILE_FORMAT] = "parquet";
    options_map["manifest.format"] = "avro";
    options_map[Options::FILE_SYSTEM] = "local";
    ASSERT_OK_AND_ASSIGN(CoreOptions options, CoreOptions::FromMap(options_map));
    auto schema =
        arrow::schema({arrow::field("embedding", arrow::fixed_size_list(arrow::float32(), 33)),
                       arrow::field("title", arrow::utf8()), arrow::field("id", arrow::int64()),
                       arrow::field("tag", arrow::utf8())});
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<TableSchema> table_schema,
                         TableSchema::Create(0, schema, {}, {}, options_map));
    ASSERT_OK_AND_ASSIGN(
        std::shared_ptr<FileStorePathFactory> paths,
        FileStorePathFactory::Create(directory->Str(), schema, {}, "", "parquet", "data-", true, {},
                                     std::nullopt, false, pool_));
    auto fs = directory->GetFileSystem();
    ASSERT_OK(fs->Mkdirs(directory->Str() + "/manifest"));
    ASSERT_OK(fs->Mkdirs(directory->Str() + "/index"));
    std::string archive_path = paths->CreateGlobalIndexFileFactory()->ToPath("eslib.index");
    std::filesystem::create_directories(std::filesystem::path(archive_path).parent_path());
    std::filesystem::copy_file(root_ + "/hnsw/archive.bin", archive_path);
    auto metadata = std::make_shared<Bytes>(Load(root_ + "/hnsw/metadata.bin"), pool_.get());
    GlobalIndexMeta global_meta(1000, 1127, 0, std::vector<int32_t>{1, 2, 3}, metadata);
    auto index_file = std::make_shared<IndexFileMeta>("es-index", "eslib.index",
                                                      std::filesystem::file_size(archive_path), 128,
                                                      std::nullopt, std::nullopt, global_meta);
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<FileFormat> format, FileFormatFactory::Get("avro", {}));
    ASSERT_OK_AND_ASSIGN(
        std::unique_ptr<IndexManifestFile> manifest,
        IndexManifestFile::Create(fs, format, "zstd", paths, options.GetBucket(), pool_, options));
    using WrittenManifest = std::pair<std::string, int64_t>;
    ASSERT_OK_AND_ASSIGN(WrittenManifest written,
                         manifest->WriteWithoutRolling({IndexManifestEntry(
                             FileKind::Add(), BinaryRow::EmptyRow(), 0, index_file)}));
    Snapshot snapshot(1, 0, "", std::nullopt, "", std::nullopt, std::nullopt, std::nullopt,
                      written.first, "test", 1, Snapshot::CommitKind::Append(), 0, 128, 128,
                      std::nullopt, std::nullopt, std::nullopt, std::nullopt, 1128);
    ASSERT_OK_AND_ASSIGN(std::unique_ptr<GlobalIndexScanImpl> scan,
                         GlobalIndexScanImpl::Create(directory->Str(), table_schema, snapshot,
                                                     nullptr, options, nullptr, pool_));
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<GlobalIndexReader> scalar,
                         scan->CreateReader("id", "es-index", std::nullopt));
    ASSERT_TRUE(scalar);
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<GlobalIndexResult> result,
                         scalar->VisitEqual(Literal(static_cast<int64_t>(9007199254741030LL))));
    ASSERT_OK_AND_ASSIGN(std::set<int64_t> rows, Rows(result));
    ASSERT_EQ(rows, std::set<int64_t>({1037}));
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<GlobalIndexReader> text,
                         scan->CreateReader("title", "es-index", std::nullopt));
    ASSERT_TRUE(text);
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<ScoredGlobalIndexResult> scored,
                         text->VisitFullTextSearch(std::make_shared<FullTextSearch>(
                             "title", "{\"match\":{\"query\":\"even\"}}", 128)));
    ASSERT_OK_AND_ASSIGN(rows, Rows(scored));
    ASSERT_EQ(rows.size(), 64u);
    ASSERT_EQ(rows.count(1036), 1u);
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<GlobalIndexReader> vector,
                         scan->CreateReader("embedding", "es-index", std::nullopt));
    ASSERT_OK_AND_ASSIGN(scored, vector->VisitVectorSearch(Search(Vector(37))));
    ASSERT_OK_AND_ASSIGN(rows, Rows(scored));
    ASSERT_EQ(rows, std::set<int64_t>({1037}));
}

TEST_F(ESLibIndexTest, ReadsJavaTemporalLongRepresentation) {
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<GlobalIndexReader> reader,
                         Open("hnsw", "day", arrow::date32()));
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<GlobalIndexResult> result,
                         reader->VisitEqual(Literal(FieldType::DATE, 20037)));
    ASSERT_OK_AND_ASSIGN(std::set<int64_t> rows, Rows(result));
    ASSERT_EQ(rows, std::set<int64_t>({37}));
    ASSERT_OK_AND_ASSIGN(reader,
                         Open("hnsw", "event_time", arrow::timestamp(arrow::TimeUnit::MILLI)));
    ASSERT_OK_AND_ASSIGN(result,
                         reader->VisitEqual(Literal(Timestamp::FromEpochMillis(1700000000037LL))));
    ASSERT_OK_AND_ASSIGN(rows, Rows(result));
    ASSERT_EQ(rows, std::set<int64_t>({37}));
}
TEST_F(ESLibIndexTest, RejectsCorruptSealedBytesAndCleansUpTemporaryFiles) {
    auto directory = UniqueTestDirectory::Create();
    ASSERT_TRUE(directory);
    auto options = options_;
    options["global-index.es-index.native.temp-directory"] = directory->Str();
    auto native_directories = [&]() {
        size_t count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(directory->Str()))
            if (entry.is_directory()) ++count;
        return count;
    };
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<GlobalIndexReader> reader,
                         Open("hnsw", "id", arrow::int64(), &options));
    ASSERT_EQ(native_directories(), 1u);
    reader.reset();
    ASSERT_EQ(native_directories(), 0u);
    std::string archive = Load(root_ + "/hnsw/archive.bin");
    size_t offset = 4;
    auto number = [&](size_t bytes) {
        uint64_t result = 0;
        for (size_t i = 0; i < bytes; ++i)
            result = (result << 8) | static_cast<uint8_t>(archive[offset++]);
        return result;
    };
    bool corrupted = false;
    while (offset < archive.size()) {
        auto length = number(4);
        std::string name = archive.substr(offset, length);
        offset += length;
        auto size = number(8);
        if (name != ".eslib-native-v1.json" && size > 0) {
            archive[offset] ^= 1;
            corrupted = true;
            break;
        }
        offset += size;
    }
    ASSERT_TRUE(corrupted);
    std::string path = directory->Str() + "/corrupt.bin";
    {
        std::ofstream stream(path, std::ios::binary);
        stream.write(archive.data(), archive.size());
    }
    GlobalIndexIOMeta meta(
        path, archive.size(),
        std::make_shared<Bytes>(Load(root_ + "/hnsw/metadata.bin"), pool_.get()));
    ASSERT_NOK_WITH_MSG(Open("hnsw", "id", arrow::int64(), &options, &meta), "SHA-256 mismatch");
    ASSERT_EQ(native_directories(), 0u);
    std::string data = Load(root_ + "/hnsw/metadata.bin");
    auto algorithm = data.find("HNSW");
    ASSERT_NE(algorithm, std::string::npos);
    data.replace(algorithm, 4, "FAIL");
    meta.file_path = root_ + "/hnsw/archive.bin";
    meta.metadata = std::make_shared<Bytes>(data, pool_.get());
    ASSERT_NOK_WITH_MSG(Open("hnsw", "id", arrow::int64(), &options, &meta), "config/dictionary");
    ASSERT_EQ(native_directories(), 0u);
}
}  // namespace paimon::test
