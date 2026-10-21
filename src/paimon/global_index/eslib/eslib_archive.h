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

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "paimon/global_index/eslib/eslib_abi.h"
#include "paimon/global_index/global_index_io_meta.h"
#include "paimon/global_index/io/global_index_file_reader.h"

namespace paimon::eslib {
struct Field {
    std::string type, algorithm, metric, analyzer, scalar_type, physical_name;
    int32_t dimension = 0;
};
// Owns the materialized sealed shard and its native reader. Initialization verifies
// the persisted backend, archive offsets, file digests and explicit runtime identity.
class Archive {
 public:
    static Result<std::unique_ptr<Archive>> Create(
        const GlobalIndexIOMeta& meta, const std::shared_ptr<GlobalIndexFileReader>& reader,
        const std::map<std::string, std::string>& options);
    ~Archive();
    Result<std::vector<abi::Hit>> Search(const std::string& json, int32_t limit,
                                         bool enumerate) const;
    const std::map<std::string, Field>& Fields() const {
        return fields_;
    }
    const std::map<std::string, std::string>& PrimaryTypes() const {
        return primary_types_;
    }
    const std::string& IndexType() const {
        return index_type_;
    }
    int64_t RowStart() const {
        return row_start_;
    }
    int64_t Rows() const {
        return rows_;
    }

 private:
    Archive() = default;
    Status Initialize(const GlobalIndexIOMeta& meta,
                      const std::shared_ptr<GlobalIndexFileReader>& reader,
                      const std::map<std::string, std::string>& options);
    std::map<std::string, Field> fields_;
    std::map<std::string, std::string> primary_types_;
    std::string directory_;
    int64_t rows_ = 0, row_start_ = 0;
    std::string index_type_ = "es-index";
    uint64_t handle_ = 0;
    void* library_ = nullptr;
    abi::QueryResult* (*query_)(uint64_t, const abi::Query*) = nullptr;
    void (*free_query_)(abi::QueryResult*) = nullptr;
    abi::Status* (*close_)(uint64_t) = nullptr;
    void (*free_status_)(abi::Status*) = nullptr;
};
}  // namespace paimon::eslib
