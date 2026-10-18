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

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "arrow/type_fwd.h"
#include "paimon/global_index/eslib/eslib_archive.h"
#include "paimon/global_index/global_index_reader.h"

namespace paimon::eslib {
class ESLibGlobalIndexReader final : public GlobalIndexReader {
 public:
    static Result<std::shared_ptr<GlobalIndexReader>> Create(
        const std::shared_ptr<arrow::Field>& field, const GlobalIndexIOMeta& meta,
        const std::shared_ptr<GlobalIndexFileReader>& reader,
        const std::map<std::string, std::string>& options);
    ~ESLibGlobalIndexReader() override = default;
    bool IsThreadSafe() const override {
        return true;
    }
    std::string GetIndexType() const override {
        return "es-index";
    }
    Result<std::shared_ptr<GlobalIndexResult>> VisitIsNull() override;
    Result<std::shared_ptr<GlobalIndexResult>> VisitIsNotNull() override;
    Result<std::shared_ptr<GlobalIndexResult>> VisitEqual(const Literal& value) override;
    Result<std::shared_ptr<GlobalIndexResult>> VisitNotEqual(const Literal& value) override;
    Result<std::shared_ptr<GlobalIndexResult>> VisitLessThan(const Literal& value) override;
    Result<std::shared_ptr<GlobalIndexResult>> VisitLessOrEqual(const Literal& value) override;
    Result<std::shared_ptr<GlobalIndexResult>> VisitGreaterThan(const Literal& value) override;
    Result<std::shared_ptr<GlobalIndexResult>> VisitGreaterOrEqual(const Literal& value) override;
    Result<std::shared_ptr<GlobalIndexResult>> VisitIn(const std::vector<Literal>& values) override;
    Result<std::shared_ptr<GlobalIndexResult>> VisitNotIn(
        const std::vector<Literal>& values) override;
    Result<std::shared_ptr<GlobalIndexResult>> VisitStartsWith(const Literal& value) override;
    Result<std::shared_ptr<GlobalIndexResult>> VisitEndsWith(const Literal& value) override;
    Result<std::shared_ptr<GlobalIndexResult>> VisitContains(const Literal& value) override;
    Result<std::shared_ptr<GlobalIndexResult>> VisitLike(const Literal& value) override;
    Result<std::shared_ptr<ScoredGlobalIndexResult>> VisitVectorSearch(
        const std::shared_ptr<VectorSearch>& search) override;
    Result<std::shared_ptr<ScoredGlobalIndexResult>> VisitFullTextSearch(
        const std::shared_ptr<FullTextSearch>& search) override;

 private:
    ESLibGlobalIndexReader(std::unique_ptr<Archive> archive, std::string field)
        : archive_(std::move(archive)), field_(std::move(field)) {}
    Result<std::shared_ptr<GlobalIndexResult>> Filter(const std::string& query);
    Result<std::shared_ptr<GlobalIndexResult>> Compare(const Literal& value,
                                                       const std::string& operation);
    Result<std::shared_ptr<GlobalIndexResult>> In(const std::vector<Literal>& values, bool negate);
    Result<std::shared_ptr<GlobalIndexResult>> Pattern(const Literal& value,
                                                       const std::string& operation);
    std::unique_ptr<Archive> archive_;
    std::string field_;
    std::mutex mutex_;
};
}  // namespace paimon::eslib
