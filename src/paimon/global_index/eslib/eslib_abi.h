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

#include <cstddef>
#include <cstdint>

// Read-only elasticpp Paimon C ABI v1. No elasticpp C++ objects cross this boundary.
namespace paimon::eslib::abi {
struct Status {
    uint32_t version, size;
    int32_t success, error, retryable, mutation_started, rebuild, reserved0;
    const char* diagnostic;
    int32_t diagnostic_length, reserved1;
    uint64_t reserved[4];
};
struct Capabilities {
    Status status;
    uint32_t minimum_version, maximum_version;
    uint64_t flags;
    uint32_t maximum_dimensions, reserved0;
    const char* commit;
    int32_t commit_length, reserved1;
    const char* build;
    int32_t build_length, reserved2;
    const char* lucene;
    int32_t lucene_length, reserved3;
    const char* target;
    int32_t target_length, reserved4;
    const char* minimum_glibc;
    int32_t minimum_glibc_length, reserved5;
    uint8_t sha256[32];
    uint64_t reserved[4];
};
struct Identity {
    uint32_t version = 1, size = sizeof(Identity);
    const char* table = nullptr;
    int32_t table_length = 0, reserved0 = 0;
    int64_t snapshot = 0, schema = 0, row_start = 0, rows = 0;
    uint8_t partition_sha256[32] = {}, columns_sha256[32] = {}, mapping_sha256[32] = {};
    uint64_t reserved[4] = {};
};
struct OpenRequest {
    uint32_t version = 1, size = sizeof(OpenRequest);
    const char* path = nullptr;
    int32_t path_length = 0, reserved0 = 0;
    const Identity* identity = nullptr;
    uint8_t directory_sha256[32] = {};
    const char* mapping = nullptr;
    int64_t mapping_length = 0;
    uint64_t reserved[4] = {};
};
struct OpenResult {
    Status status;
    uint64_t handle;
    int64_t max_doc, generation;
    uint64_t reserved[4];
};
struct Query {
    uint32_t version = 1, size = sizeof(Query);
    const char* json = nullptr;
    int64_t json_length = 0, timeout_millis = 30000, max_rows = 1000000;
    int64_t max_bytes = 64 * 1024 * 1024;
    int32_t sort_mode = 0, reserved0 = 0;
    uint64_t reserved[4] = {};
};
struct Hit {
    int64_t row;
    float score;
    uint32_t reserved0;
    uint64_t reserved[2];
};
struct QueryResult {
    Status status;
    int32_t complete, timed_out, terminated_early, approximated, relation, reserved0;
    int64_t total_hits, hit_count;
    Hit* hits;
    int64_t bytes;
    uint64_t reserved[4];
};
static_assert(sizeof(Status) == 80 && offsetof(Status, diagnostic) == 32);
static_assert(sizeof(Capabilities) == 248 && offsetof(Capabilities, sha256) == 184);
static_assert(sizeof(Identity) == 184 && offsetof(Identity, partition_sha256) == 56);
static_assert(sizeof(OpenRequest) == 112 && offsetof(OpenRequest, mapping) == 64);
static_assert(sizeof(OpenResult) == 136 && offsetof(OpenResult, handle) == 80);
static_assert(sizeof(Query) == 88 && offsetof(Query, json) == 8);
static_assert(sizeof(Hit) == 32 && offsetof(Hit, score) == 8);
static_assert(sizeof(QueryResult) == 168 && offsetof(QueryResult, hits) == 120);
}  // namespace paimon::eslib::abi
