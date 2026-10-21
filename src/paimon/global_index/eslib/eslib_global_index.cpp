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

#include <map>
#include <memory>
#include <string>
#include <utility>

#include "arrow/c/bridge.h"
#include "arrow/type.h"
#include "paimon/common/utils/arrow/status_utils.h"
#include "paimon/global_index/eslib/eslib_global_index_reader.h"
#include "paimon/global_index/global_indexer.h"
#include "paimon/global_index/global_indexer_factory.h"

namespace paimon::eslib {
namespace {
class ESLibGlobalIndex final : public GlobalIndexer {
 public:
    explicit ESLibGlobalIndex(std::map<std::string, std::string> options)
        : options_(std::move(options)) {}
    Result<std::optional<std::vector<std::string>>> GetExtraFieldNames() const override {
        return std::optional<std::vector<std::string>>();
    }
    Result<std::shared_ptr<GlobalIndexWriter>> CreateWriter(
        const std::string&, ArrowSchema*, const std::shared_ptr<GlobalIndexFileWriter>&,
        const std::shared_ptr<MemoryPool>&) const override {
        return Status::NotImplemented(
            "ESLib SDK writer is not implemented; use the Java native writer");
    }
    Result<std::shared_ptr<GlobalIndexReader>> CreateReader(
        ArrowSchema* c_schema, const std::shared_ptr<GlobalIndexFileReader>& reader,
        const std::vector<GlobalIndexIOMeta>& files,
        const std::shared_ptr<MemoryPool>&) const override {
        if (!c_schema || files.size() != 1)
            return Status::Invalid("ESLib reader needs one schema and one archive per shard");
        PAIMON_ASSIGN_OR_RAISE_FROM_ARROW(std::shared_ptr<arrow::Schema> schema,
                                          arrow::ImportSchema(c_schema));
        if (schema->num_fields() != 1)
            return Status::Invalid("Bind each SDK ESLib reader to one indexed primary field");
        return ESLibGlobalIndexReader::Create(schema->field(0), files[0], reader, options_);
    }

 private:
    const std::map<std::string, std::string> options_;
};
}  // namespace
class ESLibGlobalIndexFactory final : public GlobalIndexerFactory {
 public:
    static constexpr const char* IDENTIFIER = "es-index-global";
    const char* Identifier() const override {
        return IDENTIFIER;
    }
    Result<std::unique_ptr<GlobalIndexer>> Create(
        const std::map<std::string, std::string>& options) const override {
        auto selected = options;
        selected["native.expected-index-type"] = "es-index";
        return std::unique_ptr<GlobalIndexer>(
            std::make_unique<ESLibGlobalIndex>(std::move(selected)));
    }
};
REGISTER_PAIMON_FACTORY(ESLibGlobalIndexFactory);

class PaimonElasticsearchGlobalIndexFactory final : public GlobalIndexerFactory {
 public:
    static constexpr const char* IDENTIFIER = "paimon-elasticsearch-global";
    const char* Identifier() const override {
        return IDENTIFIER;
    }
    Result<std::unique_ptr<GlobalIndexer>> Create(
        const std::map<std::string, std::string>& options) const override {
        auto selected = options;
        selected["native.expected-index-type"] = "paimon-elasticsearch";
        return std::unique_ptr<GlobalIndexer>(
            std::make_unique<ESLibGlobalIndex>(std::move(selected)));
    }
};
REGISTER_PAIMON_FACTORY(PaimonElasticsearchGlobalIndexFactory);
}  // namespace paimon::eslib
