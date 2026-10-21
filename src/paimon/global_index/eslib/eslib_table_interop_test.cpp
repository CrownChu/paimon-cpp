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

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "arrow/api.h"
#include "gtest/gtest.h"
#include "paimon/fs/local/local_file_system.h"
#include "paimon/global_index/bitmap_scored_global_index_result.h"
#include "paimon/global_index/global_index_scan.h"
#include "paimon/predicate/predicate_builder.h"
#include "paimon/read_context.h"
#include "paimon/scan_context.h"
#include "paimon/table/source/table_read.h"
#include "paimon/table/source/table_scan.h"
#include "paimon/testing/utils/read_result_collector.h"
#include "paimon/testing/utils/testharness.h"
#include "rapidjson/document.h"

namespace paimon::test {
using ScoreMap = std::map<int64_t, float>;
namespace {
class NativeIndexCountingFileSystem final : public LocalFileSystem {
 public:
    using LocalFileSystem::Open;
    Result<std::unique_ptr<InputStream>> Open(const std::string& path) const override {
        if (path.find("es-index-") != std::string::npos ||
            path.find("paimon-elasticsearch-") != std::string::npos)
            ++native_archive_reads_;
        return LocalFileSystem::Open(path);
    }
    int32_t NativeArchiveReads() const {
        return native_archive_reads_.load();
    }

 private:
    mutable std::atomic<int32_t> native_archive_reads_{0};
};
std::shared_ptr<VectorSearch> TableVectorSearch(int32_t limit = 3) {
    std::vector<float> vector(33);
    for (size_t i = 0; i < vector.size(); ++i) vector[i] = 37 * 0.25f + i * 0.01f;
    return std::make_shared<VectorSearch>("embedding", limit, vector, nullptr, nullptr,
                                          std::nullopt, std::map<std::string, std::string>());
}
Result<ScoreMap> Scores(const std::shared_ptr<ScoredGlobalIndexResult>& result) {
    PAIMON_ASSIGN_OR_RAISE(std::unique_ptr<ScoredGlobalIndexResult::ScoredIterator> iter,
                           result->CreateScoredIterator());
    ScoreMap scores;
    while (iter->HasNext()) {
        auto [row, score] = iter->NextWithScore();
        if (!std::isfinite(score) || !scores.emplace(row, score).second) {
            return Status::Invalid("Invalid scored row returned by the SDK");
        }
    }
    return scores;
}
std::shared_ptr<ScoredGlobalIndexResult> ScoredRows(const ScoreMap& rows) {
    RoaringBitmap64 bitmap;
    std::vector<float> scores;
    for (const auto& [row, score] : rows) {
        bitmap.Add(row);
        scores.push_back(score);
    }
    return std::make_shared<BitmapScoredGlobalIndexResult>(std::move(bitmap), std::move(scores));
}
ScoreMap GlobalTopK(const ScoreMap& candidates, size_t limit) {
    std::vector<std::pair<int64_t, float>> ordered(candidates.begin(), candidates.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        return left.second != right.second ? left.second > right.second : left.first < right.first;
    });
    ScoreMap selected;
    for (size_t i = 0; i < std::min(limit, ordered.size()); ++i) selected.insert(ordered[i]);
    return selected;
}
std::set<int64_t> RowSet(const ScoreMap& rows) {
    std::set<int64_t> ids;
    for (const auto& [row, score] : rows) ids.insert(row);
    return ids;
}
}  // namespace

using TableInteropParam = std::tuple<std::string, std::string>;
class ESLibTableInteropTest : public ::testing::Test,
                              public ::testing::WithParamInterface<TableInteropParam> {
 protected:
    std::string index_type_ = "es-index";
    void SetUp() override {
        const char* root = std::getenv("PAIMON_ESLIB_TABLE_FIXTURE_DIR");
        const char* library = std::getenv("PAIMON_ESLIB_NATIVE_LIBRARY");
        const char* commit = std::getenv("PAIMON_ESLIB_NATIVE_COMMIT");
        const char* sha = std::getenv("PAIMON_ESLIB_NATIVE_SHA256");
        if (!root || !library || !commit || !sha)
            GTEST_SKIP() << "Generate committed Java table fixtures and set PAIMON_ESLIB_*";
        std::string name = std::get<0>(GetParam()) + "_" + std::get<1>(GetParam());
        std::ifstream input(std::string(root) + "/" + name + "/oracle.json");
        ASSERT_TRUE(input) << "Missing Java full-scan oracle for " << name;
        std::string json{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        oracle_.Parse(json.data(), json.size());
        ASSERT_FALSE(oracle_.HasParseError());
        ASSERT_TRUE(oracle_.IsObject());
        table_ = std::string(root) + "/" + oracle_["tablePath"].GetString();
        indexed_snapshot_ = oracle_["indexedSnapshot"].GetInt64();
        latest_snapshot_ = oracle_["latestSnapshot"].GetInt64();
        const char* selected_type = std::getenv("PAIMON_TABLE_INDEX_TYPE");
        if (selected_type) index_type_ = selected_type;
        options_ = {{Options::FILE_SYSTEM, "local"},
                    {Options::GLOBAL_INDEX_THREAD_NUM, "2"},
                    {"global-index." + index_type_ + ".native.library-path", library},
                    {"global-index." + index_type_ + ".native.commit", commit},
                    {"global-index." + index_type_ + ".native.sha256", sha}};
        fs_ = std::make_shared<NativeIndexCountingFileSystem>();
    }
    Result<std::unique_ptr<GlobalIndexScan>> ScanIndex(
        const std::optional<std::vector<std::map<std::string, std::string>>>& partitions =
            std::nullopt) {
        return GlobalIndexScan::Create(table_, indexed_snapshot_, partitions, options_, fs_,
                                       nullptr, nullptr);
    }
    Result<std::shared_ptr<arrow::ChunkedArray>> Fetch(
        const std::optional<int64_t>& snapshot,
        const std::shared_ptr<Predicate>& predicate = nullptr,
        const std::shared_ptr<GlobalIndexResult>& index_result = nullptr, bool with_score = false,
        const std::vector<std::map<std::string, std::string>>& partitions = {}) {
        auto options = options_;
        if (snapshot) {
            options[Options::SCAN_MODE] = "from-snapshot-full";
            options[Options::SCAN_SNAPSHOT_ID] = std::to_string(*snapshot);
        }
        ScanContextBuilder scan_builder(table_);
        scan_builder.SetOptions(options)
            .SetPredicate(predicate)
            .SetGlobalIndexResult(index_result)
            .WithFileSystem(fs_);
        if (!partitions.empty()) scan_builder.SetPartitionFilter(partitions);
        PAIMON_ASSIGN_OR_RAISE(std::unique_ptr<ScanContext> scan_context, scan_builder.Finish());
        PAIMON_ASSIGN_OR_RAISE(std::unique_ptr<TableScan> scan,
                               TableScan::Create(std::move(scan_context)));
        PAIMON_ASSIGN_OR_RAISE(std::shared_ptr<Plan> plan, scan->CreatePlan());
        std::vector<std::string> fields = {"embedding", "title",      "id",   "tag",
                                           "day",       "event_time", "part", "_ROW_ID"};
        if (with_score) fields.push_back("_INDEX_SCORE");
        ReadContextBuilder read_builder(table_);
        read_builder.SetOptions(options)
            .SetReadFieldNames(fields)
            .SetPredicate(predicate)
            .EnablePredicateFilter(true)
            .WithFileSystem(fs_);
        PAIMON_ASSIGN_OR_RAISE(std::unique_ptr<ReadContext> read_context, read_builder.Finish());
        PAIMON_ASSIGN_OR_RAISE(std::unique_ptr<TableRead> read,
                               TableRead::Create(std::move(read_context)));
        PAIMON_ASSIGN_OR_RAISE(std::unique_ptr<BatchReader> reader,
                               read->CreateReader(plan->Splits()));
        return ReadResultCollector::CollectResult(std::move(reader));
    }
    using RowPredicate = std::function<bool(const rapidjson::Value&)>;
    std::map<int64_t, const rapidjson::Value*> Expected(
        bool latest, const RowPredicate& filter = [](const auto&) { return true; }) const {
        std::map<int64_t, const rapidjson::Value*> rows;
        for (const auto& row : oracle_[latest ? "latestRows" : "indexedRows"].GetArray())
            if (filter(row)) rows.emplace(row["rowId"].GetInt64(), &row);
        return rows;
    }
    void AssertRows(const std::shared_ptr<arrow::ChunkedArray>& actual,
                    const std::map<int64_t, const rapidjson::Value*>& expected,
                    const ScoreMap* expected_scores = nullptr) {
        ASSERT_TRUE(actual);
        ASSERT_EQ(actual->length(), static_cast<int64_t>(expected.size()));
        std::set<int64_t> seen;
        for (const auto& chunk : actual->chunks()) {
            ASSERT_EQ(chunk->type_id(), arrow::Type::STRUCT);
            auto rows = std::static_pointer_cast<arrow::StructArray>(chunk);
            auto ids = std::static_pointer_cast<arrow::Int64Array>(rows->GetFieldByName("_ROW_ID"));
            ASSERT_TRUE(ids);
            for (int64_t position = 0; position < rows->length(); ++position) {
                int64_t id = ids->Value(position);
                SCOPED_TRACE(std::to_string(id));
                ASSERT_TRUE(seen.insert(id).second);
                auto found = expected.find(id);
                ASSERT_NE(found, expected.end());
                const auto& row = *found->second;
                auto vector = rows->GetFieldByName("embedding");
                ASSERT_EQ(vector->IsNull(position), row["embedding"].IsNull());
                if (!row["embedding"].IsNull()) {
                    std::shared_ptr<arrow::FloatArray> values;
                    int64_t offset = 0, length = 0;
                    if (vector->type_id() == arrow::Type::FIXED_SIZE_LIST) {
                        auto list = std::static_pointer_cast<arrow::FixedSizeListArray>(vector);
                        values = std::static_pointer_cast<arrow::FloatArray>(list->values());
                        offset = list->value_offset(position);
                        length = list->value_length(position);
                    } else {
                        ASSERT_EQ(vector->type_id(), arrow::Type::LIST);
                        auto list = std::static_pointer_cast<arrow::ListArray>(vector);
                        values = std::static_pointer_cast<arrow::FloatArray>(list->values());
                        offset = list->value_offset(position);
                        length = list->value_length(position);
                    }
                    ASSERT_EQ(length, row["embedding"].Size());
                    for (int64_t i = 0; i < length; ++i)
                        ASSERT_FLOAT_EQ(values->Value(offset + i), row["embedding"][i].GetFloat());
                }
                for (const char* name : {"title", "tag", "part"}) {
                    auto strings =
                        std::static_pointer_cast<arrow::StringArray>(rows->GetFieldByName(name));
                    ASSERT_EQ(strings->IsNull(position), row[name].IsNull());
                    if (!row[name].IsNull()) {
                        ASSERT_EQ(strings->GetString(position), row[name].GetString());
                    }
                }
                auto longs =
                    std::static_pointer_cast<arrow::Int64Array>(rows->GetFieldByName("id"));
                ASSERT_EQ(longs->IsNull(position), row["id"].IsNull());
                if (!row["id"].IsNull()) {
                    ASSERT_EQ(longs->Value(position), row["id"].GetInt64());
                }
                auto day =
                    std::static_pointer_cast<arrow::Date32Array>(rows->GetFieldByName("day"));
                ASSERT_EQ(day->Value(position), row["day"].GetInt());
                auto time = std::static_pointer_cast<arrow::TimestampArray>(
                    rows->GetFieldByName("event_time"));
                auto time_type = std::static_pointer_cast<arrow::TimestampType>(time->type());
                int64_t divisor = time_type->unit() == arrow::TimeUnit::NANO    ? 1000000
                                  : time_type->unit() == arrow::TimeUnit::MICRO ? 1000
                                                                                : 1;
                ASSERT_EQ(time->Value(position) / divisor, row["event_time"].GetInt64());
                if (expected_scores) {
                    auto scores = std::static_pointer_cast<arrow::FloatArray>(
                        rows->GetFieldByName("_INDEX_SCORE"));
                    ASSERT_TRUE(scores);
                    ASSERT_FALSE(scores->IsNull(position));
                    ASSERT_NEAR(scores->Value(position), expected_scores->at(id), 1e-6f);
                }
            }
        }
        ASSERT_EQ(seen.size(), expected.size());
    }
    ScoreMap ExactVectorScores(
        const std::shared_ptr<VectorSearch>& search,
        const RowPredicate& filter = [](const auto&) { return true; }) const {
        ScoreMap scores;
        for (const auto& [id, row] : Expected(false, filter)) {
            if ((*row)["embedding"].IsNull()) continue;
            double distance = 0;
            for (size_t i = 0; i < search->query.size(); ++i) {
                double difference =
                    static_cast<double>((*row)["embedding"][i].GetFloat()) - search->query[i];
                distance += difference * difference;
            }
            scores[id] = 1.0 / (1.0 + distance);
        }
        return GlobalTopK(scores, search->limit);
    }
    rapidjson::Document oracle_;
    std::string table_;
    int64_t indexed_snapshot_ = 0, latest_snapshot_ = 0;
    std::map<std::string, std::string> options_;
    std::shared_ptr<NativeIndexCountingFileSystem> fs_;
};

TEST_P(ESLibTableInteropTest, ReadsJavaCommittedSnapshotsAndPartitions) {
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<arrow::ChunkedArray> data, Fetch(indexed_snapshot_));
    AssertRows(data, Expected(false));
    ASSERT_OK_AND_ASSIGN(data, Fetch(std::nullopt));
    AssertRows(data, Expected(true));
    ASSERT_OK_AND_ASSIGN(data,
                         Fetch(indexed_snapshot_, nullptr, nullptr, false, {{{"part", "p1"}}}));
    AssertRows(data, Expected(false, [](const auto& row) {
                   return std::string(row["part"].GetString()) == "p1";
               }));
}
TEST_P(ESLibTableInteropTest, ScalarNativeQueriesRetainUnindexedAppendAndNulls) {
    auto equal = PredicateBuilder::Equal(2, "id", FieldType::BIGINT,
                                         Literal(static_cast<int64_t>(9007199254741030LL)));
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<arrow::ChunkedArray> data,
                         Fetch(indexed_snapshot_, equal));
    AssertRows(data, Expected(false, [](const auto& row) {
                   return !row["id"].IsNull() && row["id"].GetInt64() == 9007199254741030LL;
               }));
    ASSERT_GT(fs_->NativeArchiveReads(), 0) << "Scalar query silently bypassed native indexing";
    const int32_t indexed_reads = fs_->NativeArchiveReads();
    auto greater = PredicateBuilder::GreaterThan(2, "id", FieldType::BIGINT,
                                                 Literal(static_cast<int64_t>(9007199254741118LL)));
    ASSERT_OK_AND_ASSIGN(data, Fetch(latest_snapshot_, greater));
    AssertRows(data, Expected(true, [](const auto& row) {
                   return !row["id"].IsNull() && row["id"].GetInt64() > 9007199254741118LL;
               }));
    ASSERT_OK_AND_ASSIGN(data,
                         Fetch(std::nullopt, PredicateBuilder::IsNull(2, "id", FieldType::BIGINT)));
    AssertRows(data, Expected(true, [](const auto& row) { return row["id"].IsNull(); }));
    auto appended = PredicateBuilder::Equal(2, "id", FieldType::BIGINT,
                                            Literal(static_cast<int64_t>(9007199254741128LL)));
    ASSERT_OK_AND_ASSIGN(data, Fetch(std::nullopt, appended));
    AssertRows(data, Expected(true, [](const auto& row) {
                   return !row["id"].IsNull() && row["id"].GetInt64() == 9007199254741128LL;
               }));
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<Predicate> either,
                         PredicateBuilder::Or({equal, appended}));
    ASSERT_OK_AND_ASSIGN(data, Fetch(std::nullopt, either));
    AssertRows(data, Expected(true, [](const auto& row) {
                   return !row["id"].IsNull() && (row["id"].GetInt64() == 9007199254741030LL ||
                                                  row["id"].GetInt64() == 9007199254741128LL);
               }));
    auto not_equal = PredicateBuilder::NotEqual(2, "id", FieldType::BIGINT,
                                                Literal(static_cast<int64_t>(9007199254741030LL)));
    ASSERT_OK_AND_ASSIGN(data, Fetch(std::nullopt, not_equal));
    AssertRows(data, Expected(true, [](const auto& row) {
                   return !row["id"].IsNull() && row["id"].GetInt64() != 9007199254741030LL;
               }));
    ASSERT_EQ(fs_->NativeArchiveReads(), indexed_reads)
        << "Incomplete index coverage must fall back before pruning new rows";
    auto wrong = options_;
    wrong["global-index." + index_type_ + ".native.sha256"] = std::string(64, '0');
    options_ = wrong;
    ASSERT_NOK_WITH_MSG(Fetch(indexed_snapshot_, equal), "must match");
}
TEST_P(ESLibTableInteropTest, VectorTopKReadsCompleteRowsAndScores) {
    ASSERT_OK_AND_ASSIGN(std::unique_ptr<GlobalIndexScan> scan, ScanIndex());
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<GlobalIndexReader> reader,
                         scan->CreateReader("embedding", index_type_, std::nullopt));
    ASSERT_TRUE(reader);
    auto search = TableVectorSearch();
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<ScoredGlobalIndexResult> result,
                         reader->VisitVectorSearch(search));
    ASSERT_OK_AND_ASSIGN(ScoreMap candidates, Scores(result));
    auto selected = GlobalTopK(candidates, search->limit);
    auto expected = ExactVectorScores(search);
    ASSERT_EQ(RowSet(selected), RowSet(expected));
    for (const auto& [id, score] : selected) ASSERT_NEAR(score, expected.at(id), 1e-5f);
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<arrow::ChunkedArray> data,
                         Fetch(indexed_snapshot_, nullptr, ScoredRows(selected), true));
    AssertRows(
        data,
        Expected(false,
                 [&](const auto& row) { return selected.count(row["rowId"].GetInt64()) != 0; }),
        &selected);
}
TEST_P(ESLibTableInteropTest, VectorGlobalPrefilterCrossesShardOffsets) {
    ASSERT_OK_AND_ASSIGN(std::unique_ptr<GlobalIndexScan> scan, ScanIndex());
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<GlobalIndexReader> reader,
                         scan->CreateReader("embedding", index_type_, std::nullopt));
    ASSERT_TRUE(reader);
    auto search = TableVectorSearch(1);
    search->pre_filter = [](int64_t row) { return row >= 64 && row % 2 == 0; };
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<ScoredGlobalIndexResult> result,
                         reader->VisitVectorSearch(search));
    ASSERT_OK_AND_ASSIGN(ScoreMap candidates, Scores(result));
    auto selected = GlobalTopK(candidates, 1);
    auto expected = ExactVectorScores(search, [](const auto& row) {
        return row["rowId"].GetInt64() >= 64 && row["rowId"].GetInt64() % 2 == 0;
    });
    ASSERT_EQ(RowSet(selected), RowSet(expected));
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<arrow::ChunkedArray> data,
                         Fetch(indexed_snapshot_, nullptr, ScoredRows(selected), true));
    AssertRows(
        data,
        Expected(false,
                 [&](const auto& row) { return selected.count(row["rowId"].GetInt64()) != 0; }),
        &selected);
}
TEST_P(ESLibTableInteropTest, FullTextAndIncludeIdsReadCompleteRows) {
    ASSERT_OK_AND_ASSIGN(std::unique_ptr<GlobalIndexScan> scan, ScanIndex());
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<GlobalIndexReader> reader,
                         scan->CreateReader("title", index_type_, std::nullopt));
    ASSERT_TRUE(reader);
    auto search =
        std::make_shared<FullTextSearch>("title", "{\"match\":{\"query\":\"even\"}}", 256);
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<ScoredGlobalIndexResult> result,
                         reader->VisitFullTextSearch(search));
    ASSERT_OK_AND_ASSIGN(ScoreMap scores, Scores(result));
    auto expected = Expected(false, [](const auto& row) {
        return !row["title"].IsNull() &&
               std::string(row["title"].GetString()).find("even") != std::string::npos;
    });
    ASSERT_EQ(scores.size(), expected.size());
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<arrow::ChunkedArray> data,
                         Fetch(indexed_snapshot_, nullptr, result, true));
    AssertRows(data, expected, &scores);
    RoaringBitmap64 included;
    included.Add(31);
    included.Add(64);
    included.Add(96);
    included.Add(999);
    search->include_row_ids = included;
    ASSERT_OK_AND_ASSIGN(result, reader->VisitFullTextSearch(search));
    ASSERT_OK_AND_ASSIGN(scores, Scores(result));
    ASSERT_EQ(RowSet(scores), std::set<int64_t>({64, 96}));
    ASSERT_OK_AND_ASSIGN(data, Fetch(indexed_snapshot_, nullptr, result, true));
    AssertRows(data,
               Expected(false,
                        [](const auto& row) {
                            int64_t id = row["rowId"].GetInt64();
                            return id == 64 || id == 96;
                        }),
               &scores);
}
INSTANTIATE_TEST_SUITE_P(JavaCommittedTables, ESLibTableInteropTest,
                         ::testing::Combine(::testing::Values("parquet", "orc"),
                                            ::testing::Values("hnsw", "int8_hnsw", "diskbbq",
                                                              "hnswq")));
}  // namespace paimon::test
