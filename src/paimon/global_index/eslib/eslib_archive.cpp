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

#include "paimon/global_index/eslib/eslib_archive.h"

#include <cpuid.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <utility>

#include "openssl/evp.h"
#include "paimon/common/utils/scope_guard.h"
#include "paimon/fs/file_system.h"
#include "paimon/memory/bytes.h"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

namespace paimon::eslib {
namespace {
constexpr const char* kManifest = ".eslib-native-v1.json";
constexpr int64_t kMaxMetadata = 32 * 1024 * 1024;

class Digest {
 public:
    Digest() : context_(EVP_MD_CTX_new(), EVP_MD_CTX_free) {}
    Status Init() {
        if (!context_ || EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) != 1) {
            return Status::IOError("Cannot initialize SHA-256");
        }
        return Status::OK();
    }
    Status Add(const void* data, size_t size) {
        if (EVP_DigestUpdate(context_.get(), data, size) != 1) {
            return Status::IOError("Cannot update SHA-256");
        }
        return Status::OK();
    }
    Result<std::string> Finish() {
        std::array<unsigned char, 32> bytes{};
        uint32_t size = 0;
        if (EVP_DigestFinal_ex(context_.get(), bytes.data(), &size) != 1 || size != 32) {
            return Status::IOError("Cannot finish SHA-256");
        }
        std::string hex;
        for (uint8_t byte : bytes) {
            hex += "0123456789abcdef"[byte >> 4];
            hex += "0123456789abcdef"[byte & 15];
        }
        return hex;
    }

 private:
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context_;
};
Result<std::string> Sha(const std::string& value) {
    Digest digest;
    PAIMON_RETURN_NOT_OK(digest.Init());
    PAIMON_RETURN_NOT_OK(digest.Add(value.data(), value.size()));
    return digest.Finish();
}
bool IsHex(const std::string& value, size_t length) {
    return value.size() == length &&
           value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
Status Unhex(const std::string& hex, uint8_t* output) {
    if (!IsHex(hex, 64)) return Status::Invalid("Invalid SHA-256 identity");
    for (size_t i = 0; i < 32; ++i) {
        auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        output[i] = (digit(hex[i * 2]) << 4) | digit(hex[i * 2 + 1]);
    }
    return Status::OK();
}
bool SafeName(const std::string& name) {
    return !name.empty() && name.size() <= 4096 && name != "." && name != ".." &&
           name.find_first_of("/\\") == std::string::npos && name.find('\0') == std::string::npos;
}
class Cursor {
 public:
    explicit Cursor(const std::string& data) : data_(data) {}
    Result<uint64_t> Number(size_t bytes) {
        if (bytes > data_.size() - offset_) return Status::Invalid("Truncated ESM1 metadata");
        uint64_t value = 0;
        for (size_t i = 0; i < bytes; ++i)
            value = (value << 8) | static_cast<uint8_t>(data_[offset_++]);
        return value;
    }
    Result<int32_t> Count() {
        PAIMON_ASSIGN_OR_RAISE(uint64_t value, Number(4));
        if (value > 100000 || value > data_.size() - offset_) {
            return Status::Invalid("Invalid ESM1 count");
        }
        return static_cast<int32_t>(value);
    }
    Result<std::string> String() {
        PAIMON_ASSIGN_OR_RAISE(uint64_t size, Number(4));
        if (size > 1024 * 1024 || size > data_.size() - offset_) {
            return Status::Invalid("Invalid ESM1 string length");
        }
        std::string value = data_.substr(offset_, size);
        offset_ += size;
        if (value.find('\0') != std::string::npos) return Status::Invalid("NUL in ESM1 string");
        return value;
    }
    Result<std::string> NullableString() {
        PAIMON_ASSIGN_OR_RAISE(uint64_t present, Number(1));
        if (present > 1) return Status::Invalid("Invalid ESM1 nullable flag");
        if (present == 0) return std::string();
        return String();
    }
    bool Done() const {
        return offset_ == data_.size();
    }

 private:
    const std::string& data_;
    size_t offset_ = 0;
};
Status ReadExact(InputStream* stream, char* output, int64_t length, int64_t offset) {
    while (length > 0) {
        PAIMON_ASSIGN_OR_RAISE(int64_t count, stream->Read(output, length, offset));
        if (count <= 0 || count > length) return Status::IOError("Truncated ESLib archive");
        output += count;
        length -= count;
        offset += count;
    }
    return Status::OK();
}
Result<std::string> Read(InputStream* stream, int64_t offset, int64_t length) {
    std::string data(length, '\0');
    PAIMON_RETURN_NOT_OK(ReadExact(stream, data.data(), length, offset));
    return data;
}
Status CheckJson(const rapidjson::Value& value, int32_t depth = 0) {
    if (depth > 64) return Status::Invalid("ESLib JSON nesting exceeds limit");
    if (value.IsObject()) {
        std::set<std::string> keys;
        for (auto it = value.MemberBegin(); it != value.MemberEnd(); ++it) {
            if (!keys.emplace(it->name.GetString(), it->name.GetStringLength()).second) {
                return Status::Invalid("Duplicate ESLib JSON member");
            }
            PAIMON_RETURN_NOT_OK(CheckJson(it->value, depth + 1));
        }
    } else if (value.IsArray()) {
        for (const auto& entry : value.GetArray())
            PAIMON_RETURN_NOT_OK(CheckJson(entry, depth + 1));
    }
    return Status::OK();
}
Result<std::string> Text(const rapidjson::Value& node, const char* key) {
    if (!node.IsObject() || !node.HasMember(key) || !node[key].IsString()) {
        return Status::Invalid("Missing ESLib string member: ", key);
    }
    std::string result(node[key].GetString(), node[key].GetStringLength());
    if (result.find('\0') != std::string::npos) return Status::Invalid("NUL in ESLib identity");
    return result;
}
Status NativeStatus(const abi::Status& status) {
    if (status.version != 1 || status.size != sizeof(abi::Status)) {
        return Status::Invalid("Incompatible elasticpp status ABI");
    }
    if (status.success == 1 && status.error == 0) return Status::OK();
    std::string message;
    if (status.diagnostic && status.diagnostic_length > 0 && status.diagnostic_length <= 65536) {
        message.assign(status.diagnostic, status.diagnostic_length);
    }
    return Status::IOError("elasticpp error ", status.error, ": ", message);
}
template <typename T>
Status Symbol(void* library, const char* name, T* output) {
    *output = reinterpret_cast<T>(dlsym(library, name));
    return *output ? Status::OK() : Status::Invalid("Missing elasticpp C ABI symbol: ", name);
}
std::string JsonString(const std::string& text) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.String(text.data(), text.size());
    return {buffer.GetString(), buffer.GetSize()};
}
Result<std::string> ConfigDigest(const std::map<std::string, Field>& fields,
                                 std::string* selected_dictionary) {
    std::map<std::string, std::string> configs;
    bool ik = false;
    auto nullable = [](const std::string& value) {
        return value.empty() ? "null" : JsonString(value);
    };
    for (const auto& [name, field] : fields) {
        configs[name] = "{\"algorithm\":" + nullable(field.algorithm) +
                        ",\"analyzer\":" + nullable(field.analyzer) +
                        ",\"dimension\":" + std::to_string(field.dimension) +
                        ",\"metric\":" + nullable(field.metric) +
                        ",\"parameters\":{},\"scalarType\":" + nullable(field.scalar_type) +
                        ",\"type\":" + JsonString(field.type) + '}';
        ik = ik || field.analyzer == "IK_SMART" || field.analyzer == "IK_MAX_WORD";
    }
    if (ik) {
        const char* config_path = std::getenv("ES_PATH_CONF");
        if (!config_path || !*config_path)
            return Status::Invalid("IK requires ES_PATH_CONF/analysis-ik dictionaries");
        std::string dictionary = "{";
        for (const char* name : {"main.dic", "quantifier.dic", "stopword.dic"}) {
            std::filesystem::path path = std::filesystem::path(config_path) / "analysis-ik" / name;
            std::error_code error;
            auto size = std::filesystem::file_size(path, error);
            if (error || size > 64 * 1024 * 1024 || (std::string(name) == "main.dic" && size == 0))
                return Status::Invalid("Missing, empty or oversized IK dictionary");
            std::ifstream input(path, std::ios::binary);
            std::string bytes(size, '\0');
            if (!input.read(bytes.data(), size))
                return Status::IOError("Cannot read IK dictionary");
            PAIMON_ASSIGN_OR_RAISE(std::string sha, Sha(bytes));
            if (dictionary.size() > 1) dictionary += ',';
            dictionary += JsonString(name) + ':' + JsonString(sha);
        }
        dictionary += '}';
        *selected_dictionary = dictionary;
        configs["__native_ik_dictionary"] = dictionary;
    }
    std::string json = "{";
    for (const auto& [name, config] : configs) {
        if (json.size() > 1) json += ',';
        json += JsonString(name) + ':' + config;
    }
    return Sha(json + '}');
}
struct Offset {
    int64_t offset, length;
};
}  // namespace

Result<std::unique_ptr<Archive>> Archive::Create(
    const GlobalIndexIOMeta& meta, const std::shared_ptr<GlobalIndexFileReader>& reader,
    const std::map<std::string, std::string>& options) {
    auto result = std::unique_ptr<Archive>(new Archive());
    PAIMON_RETURN_NOT_OK(result->Initialize(meta, reader, options));
    return result;
}
Archive::~Archive() {
    if (handle_ != 0 && close_ && free_status_) free_status_(close_(handle_));
    // RTLD_NODELETE retains engine globals until process shutdown. Close the loader reference.
    if (library_) dlclose(library_);
    if (!directory_.empty()) {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }
}
Status Archive::Initialize(const GlobalIndexIOMeta& meta,
                           const std::shared_ptr<GlobalIndexFileReader>& reader,
                           const std::map<std::string, std::string>& options) {
    if (!reader || !meta.metadata || meta.metadata->size() > kMaxMetadata || meta.file_size < 4) {
        return Status::Invalid("Missing or oversized native ESLib metadata/archive");
    }
    std::string data(meta.metadata->data(), meta.metadata->size());
    Cursor cursor(data);
    PAIMON_ASSIGN_OR_RAISE(uint64_t magic, cursor.Number(4));
    PAIMON_ASSIGN_OR_RAISE(uint64_t version, cursor.Number(4));
    if (magic != 0x45534d31 || version != 3) {
        return Status::Invalid("Native ESLib reader requires ESM1 v3; Java archives require Java");
    }
    PAIMON_ASSIGN_OR_RAISE(std::string backend, cursor.String());
    if (backend != "elasticpp") return Status::Invalid("ESM1 backend must be elasticpp");
    PAIMON_ASSIGN_OR_RAISE(int32_t count, cursor.Count());
    if (count < 1 || count > 1024) return Status::Invalid("Invalid ESLib primary field count");
    for (int32_t i = 0; i < count; ++i) {
        PAIMON_ASSIGN_OR_RAISE(std::string name, cursor.String());
        PAIMON_ASSIGN_OR_RAISE(std::string type, cursor.String());
        if (name.empty() || !primary_types_.emplace(name, type).second) {
            return Status::Invalid("Duplicate or empty ESLib primary field");
        }
    }
    PAIMON_ASSIGN_OR_RAISE(int32_t fields, cursor.Count());
    if (fields < 1 || fields > 1024) return Status::Invalid("Invalid ESLib field count");
    for (int32_t i = 0; i < fields; ++i) {
        PAIMON_ASSIGN_OR_RAISE(std::string name, cursor.String());
        Field field;
        PAIMON_ASSIGN_OR_RAISE(field.type, cursor.String());
        PAIMON_ASSIGN_OR_RAISE(field.algorithm, cursor.NullableString());
        PAIMON_ASSIGN_OR_RAISE(uint64_t dimension, cursor.Number(4));
        if (dimension > 1024) return Status::Invalid("Invalid ESLib vector dimension");
        field.dimension = dimension;
        PAIMON_ASSIGN_OR_RAISE(field.metric, cursor.NullableString());
        PAIMON_ASSIGN_OR_RAISE(field.analyzer, cursor.NullableString());
        PAIMON_ASSIGN_OR_RAISE(field.scalar_type, cursor.NullableString());
        PAIMON_ASSIGN_OR_RAISE(int32_t parameters, cursor.Count());
        if (parameters != 0)
            return Status::Invalid("Unsupported native ESLib algorithm parameters");
        if (name.empty() || name[0] == '_' || !fields_.emplace(name, std::move(field)).second) {
            return Status::Invalid("Invalid native ESLib field name");
        }
    }
    for (const auto& [name, type] : primary_types_) {
        if (!fields_.count(name)) return Status::Invalid("Missing ESLib primary field config");
    }
    PAIMON_ASSIGN_OR_RAISE(int32_t files, cursor.Count());
    if (files < 2) return Status::Invalid("Native ESLib archive needs manifest and sealed files");
    std::map<std::string, Offset> offsets;
    for (int32_t i = 0; i < files; ++i) {
        PAIMON_ASSIGN_OR_RAISE(std::string name, cursor.String());
        PAIMON_ASSIGN_OR_RAISE(uint64_t offset, cursor.Number(8));
        PAIMON_ASSIGN_OR_RAISE(uint64_t length, cursor.Number(8));
        if (!SafeName(name) || offset > static_cast<uint64_t>(meta.file_size) ||
            length > static_cast<uint64_t>(meta.file_size) - offset ||
            !offsets
                 .emplace(name, Offset{static_cast<int64_t>(offset), static_cast<int64_t>(length)})
                 .second) {
            return Status::Invalid("Invalid native ESLib archive range/name");
        }
    }
    if (!cursor.Done() || !offsets.count(kManifest)) {
        return Status::Invalid("Trailing ESM1 data or missing native ESLib manifest");
    }
    PAIMON_ASSIGN_OR_RAISE(std::unique_ptr<InputStream> stream,
                           reader->GetInputStream(meta.file_path));
    if (!stream) return Status::Invalid("Null native ESLib archive stream");
    ScopeGuard close_stream([&]() { (void)stream->Close(); });
    PAIMON_ASSIGN_OR_RAISE(int64_t actual_size, stream->Length());
    if (actual_size != meta.file_size) return Status::Invalid("ESLib archive length mismatch");
    PAIMON_ASSIGN_OR_RAISE(std::string first, Read(stream.get(), 0, 4));
    Cursor header(first);
    PAIMON_ASSIGN_OR_RAISE(uint64_t archive_files, header.Number(4));
    if (archive_files != static_cast<uint64_t>(files))
        return Status::Invalid("ESLib archive count mismatch");
    int64_t position = 4;
    std::set<std::string> seen;
    for (int32_t i = 0; i < files; ++i) {
        if (meta.file_size - position < 4) return Status::Invalid("Truncated ESLib archive header");
        PAIMON_ASSIGN_OR_RAISE(std::string size_bytes, Read(stream.get(), position, 4));
        Cursor size_cursor(size_bytes);
        PAIMON_ASSIGN_OR_RAISE(uint64_t size, size_cursor.Number(4));
        position += 4;
        if (size == 0 || size > 4096 ||
            static_cast<uint64_t>(meta.file_size - position) < size + 8) {
            return Status::Invalid("Invalid ESLib archive file header");
        }
        PAIMON_ASSIGN_OR_RAISE(std::string name, Read(stream.get(), position, size));
        position += size;
        PAIMON_ASSIGN_OR_RAISE(std::string length_bytes, Read(stream.get(), position, 8));
        Cursor length_cursor(length_bytes);
        PAIMON_ASSIGN_OR_RAISE(uint64_t length, length_cursor.Number(8));
        position += 8;
        auto entry = offsets.find(name);
        if (entry == offsets.end() || !seen.insert(name).second ||
            entry->second.offset != position ||
            static_cast<uint64_t>(entry->second.length) != length) {
            return Status::Invalid("ESM1 offsets differ from archive headers");
        }
        position += entry->second.length;
    }
    if (position != meta.file_size) return Status::Invalid("Trailing ESLib archive bytes");
    auto descriptor = offsets.at(kManifest);
    if (descriptor.length > 16 * 1024 * 1024)
        return Status::Invalid("ESLib manifest exceeds limit");
    PAIMON_ASSIGN_OR_RAISE(std::string manifest_bytes,
                           Read(stream.get(), descriptor.offset, descriptor.length));
    rapidjson::Document manifest;
    manifest.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag>(
        manifest_bytes.data(), manifest_bytes.size());
    if (manifest.HasParseError() || !manifest.IsObject() || manifest.MemberCount() != 10) {
        return Status::Invalid("Invalid native ESLib manifest JSON");
    }
    PAIMON_RETURN_NOT_OK(CheckJson(manifest));
    PAIMON_ASSIGN_OR_RAISE(std::string format, Text(manifest, "format"));
    PAIMON_ASSIGN_OR_RAISE(std::string identity_name, Text(manifest, "identity"));
    PAIMON_ASSIGN_OR_RAISE(std::string mapping, Text(manifest, "mapping"));
    PAIMON_ASSIGN_OR_RAISE(std::string mapping_sha, Text(manifest, "mappingSha256"));
    PAIMON_ASSIGN_OR_RAISE(std::string columns_sha, Text(manifest, "configSha256"));
    PAIMON_ASSIGN_OR_RAISE(std::string native_commit, Text(manifest, "nativeCommit"));
    PAIMON_ASSIGN_OR_RAISE(std::string native_sha, Text(manifest, "nativeSha256"));
    PAIMON_ASSIGN_OR_RAISE(std::string directory_sha, Text(manifest, "directorySha256"));
    PAIMON_ASSIGN_OR_RAISE(std::string computed_mapping_sha, Sha(mapping));
    if (format != "eslib-native-index-v1" || identity_name.empty() || identity_name.size() > 1024 ||
        mapping.size() > 8 * 1024 * 1024 || mapping_sha != computed_mapping_sha ||
        !IsHex(native_commit, 40) || !IsHex(native_sha, 64) || !IsHex(columns_sha, 64) ||
        !IsHex(directory_sha, 64) || !manifest.HasMember("rows") || !manifest["rows"].IsInt64() ||
        manifest["rows"].GetInt64() < 0 || manifest["rows"].GetInt64() > 2147483519) {
        return Status::Invalid("Invalid native ESLib manifest identity/digest");
    }
    rows_ = manifest["rows"].GetInt64();
    std::string selected_dictionary;
    PAIMON_ASSIGN_OR_RAISE(std::string actual_config_sha,
                           ConfigDigest(fields_, &selected_dictionary));
    if (actual_config_sha != columns_sha)
        return Status::Invalid("ESLib config/dictionary SHA-256 mismatch");
    if (!selected_dictionary.empty()) {
        static std::mutex dictionary_mutex;
        static std::string pinned_dictionary;
        std::lock_guard<std::mutex> lock(dictionary_mutex);
        if (!pinned_dictionary.empty() && pinned_dictionary != selected_dictionary) {
            return Status::Invalid("IK dictionaries changed; use a fresh process");
        }
        pinned_dictionary = selected_dictionary;
    }
    rapidjson::Document mapping_json;
    mapping_json.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag>(
        mapping.data(), mapping.size());
    if (mapping_json.HasParseError() || !mapping_json.IsObject() ||
        !mapping_json.HasMember("properties") || !mapping_json["properties"].IsObject() ||
        !mapping_json.HasMember("_source") || !mapping_json["_source"].IsObject() ||
        !mapping_json["_source"].HasMember("enabled") ||
        !mapping_json["_source"]["enabled"].IsFalse()) {
        return Status::Invalid("Invalid native ESLib physical mapping");
    }
    PAIMON_RETURN_NOT_OK(CheckJson(mapping_json));
    const auto& properties = mapping_json["properties"];
    if (properties.MemberCount() != fields_.size())
        return Status::Invalid("ESLib mapping field count mismatch");
    std::set<std::string> physical_names;
    for (auto& [name, field] : fields_) {
        field.physical_name = name;
        if (name.find('.') != std::string::npos) {
            PAIMON_ASSIGN_OR_RAISE(std::string digest, Sha(name));
            std::string base = "eslib_" + digest;
            field.physical_name = base;
            for (int32_t suffix = 1;
                 fields_.count(field.physical_name) || physical_names.count(field.physical_name);
                 ++suffix) {
                field.physical_name = base + "_" + std::to_string(suffix);
            }
        }
        physical_names.insert(field.physical_name);
        if (!properties.HasMember(field.physical_name.c_str()))
            return Status::Invalid("Missing ESLib mapped field");
        const auto& physical = properties[field.physical_name.c_str()];
        PAIMON_ASSIGN_OR_RAISE(std::string type, Text(physical, "type"));
        if (field.type == "VECTOR") {
            PAIMON_ASSIGN_OR_RAISE(std::string similarity, Text(physical, "similarity"));
            std::string metric = field.metric;
            if (metric == "l2" || metric == "euclidean") metric = "l2_norm";
            if (metric == "ip") metric = "dot_product";
            std::string algorithm = field.algorithm == "HNSW"        ? "hnsw"
                                    : field.algorithm == "INT8_HNSW" ? "int8_hnsw"
                                    : field.algorithm == "DISKBBQ"   ? "bbq_disk"
                                                                     : "";
            if (type != "dense_vector" || field.dimension < 1 || !physical.HasMember("dims") ||
                !physical["dims"].IsInt() || physical["dims"].GetInt() != field.dimension ||
                similarity != metric || algorithm.empty() || !physical.HasMember("index_options") ||
                !physical["index_options"].IsObject())
                return Status::Invalid("ESLib vector mapping mismatch");
            PAIMON_ASSIGN_OR_RAISE(std::string mapped_algorithm,
                                   Text(physical["index_options"], "type"));
            if (algorithm != mapped_algorithm)
                return Status::Invalid("ESLib vector algorithm mismatch");
        } else if (field.type == "FULLTEXT") {
            if (type != "text") return Status::Invalid("ESLib text mapping mismatch");
            PAIMON_ASSIGN_OR_RAISE(std::string analyzer, Text(physical, "analyzer"));
            std::string expected = field.analyzer;
            std::transform(expected.begin(), expected.end(), expected.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (expected.empty()) expected = "standard";
            if (analyzer != expected) return Status::Invalid("ESLib analyzer mismatch");
        } else {
            std::string expected = field.type == "KEYWORD"          ? "keyword"
                                   : field.type == "DATE"           ? "date"
                                   : field.scalar_type == "INT"     ? "integer"
                                   : field.scalar_type == "LONG"    ? "long"
                                   : field.scalar_type == "FLOAT"   ? "float"
                                   : field.scalar_type == "DOUBLE"  ? "double"
                                   : field.scalar_type == "KEYWORD" ? "keyword"
                                   : field.scalar_type == "DATE"    ? "date"
                                                                    : "";
            if (expected.empty() || type != expected)
                return Status::Invalid("ESLib scalar mapping mismatch");
        }
    }
    auto option = [&](const char* key) -> std::string {
        auto it = options.find(key);
        return it == options.end() ? std::string() : it->second;
    };
    std::string library_path = option("global-index.es-index.native.library-path");
    if (library_path.empty() || library_path[0] != '/' ||
        option("global-index.es-index.native.sha256") != native_sha ||
        option("global-index.es-index.native.commit") != native_commit) {
        return Status::Invalid(
            "Explicit ESLib native library-path, sha256 and commit must match the archive");
    }
    std::ifstream library_input(library_path, std::ios::binary);
    if (!library_input) return Status::IOError("Cannot read configured elasticpp library");
    Digest library_digest;
    PAIMON_RETURN_NOT_OK(library_digest.Init());
    std::array<char, 1024 * 1024> buffer{};
    while (library_input) {
        library_input.read(buffer.data(), buffer.size());
        PAIMON_RETURN_NOT_OK(library_digest.Add(buffer.data(), library_input.gcount()));
    }
    if (!library_input.eof()) return Status::IOError("Cannot hash configured elasticpp library");
    PAIMON_ASSIGN_OR_RAISE(std::string actual_sha, library_digest.Finish());
    if (actual_sha != native_sha) return Status::Invalid("Configured elasticpp SO digest mismatch");
#if defined(__linux__) && defined(__x86_64__)
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    bool v3_scalar =
        __get_cpuid(1, &eax, &ebx, &ecx, &edx) && (ecx & (1U << 22)) && (ecx & (1U << 29));
    v3_scalar = v3_scalar && __get_cpuid(0x80000001, &eax, &ebx, &ecx, &edx) && (ecx & (1U << 5));
    if (!v3_scalar || !__builtin_cpu_supports("avx2") || !__builtin_cpu_supports("fma") ||
        !__builtin_cpu_supports("bmi2") || !__builtin_cpu_supports("bmi")) {
        return Status::Invalid("elasticpp runtime requires x86-64-v3 CPU support");
    }
#else
    return Status::Invalid("This native ESLib runtime requires Linux x86_64");
#endif
    library_ = dlopen(library_path.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
    if (!library_) return Status::IOError("Cannot load configured elasticpp library: ", dlerror());
    abi::Capabilities* (*capabilities)() = nullptr;
    void (*free_capabilities)(abi::Capabilities*) = nullptr;
    abi::OpenResult* (*open)(const abi::OpenRequest*) = nullptr;
    void (*free_open)(abi::OpenResult*) = nullptr;
    PAIMON_RETURN_NOT_OK(Symbol(library_, "getNativeEsCapabilitiesV1", &capabilities));
    PAIMON_RETURN_NOT_OK(Symbol(library_, "freeNativeEsCapabilitiesV1", &free_capabilities));
    PAIMON_RETURN_NOT_OK(Symbol(library_, "openArchivedShardV1", &open));
    PAIMON_RETURN_NOT_OK(Symbol(library_, "freePaimonEsOpenArchivedShardResultV1", &free_open));
    PAIMON_RETURN_NOT_OK(Symbol(library_, "queryArchivedShardV1", &query_));
    PAIMON_RETURN_NOT_OK(Symbol(library_, "freePaimonEsArchivedQueryResultV1", &free_query_));
    PAIMON_RETURN_NOT_OK(Symbol(library_, "closeArchivedShardV1", &close_));
    PAIMON_RETURN_NOT_OK(Symbol(library_, "freePaimonEsStatusV1", &free_status_));
    std::unique_ptr<abi::Capabilities, decltype(free_capabilities)> caps(capabilities(),
                                                                         free_capabilities);
    if (!caps) return Status::IOError("Null elasticpp capabilities");
    PAIMON_RETURN_NOT_OK(NativeStatus(caps->status));
    std::array<uint8_t, 32> expected_sha{};
    PAIMON_RETURN_NOT_OK(Unhex(native_sha, expected_sha.data()));
    if (caps->minimum_version > 1 || caps->maximum_version < 1 || !(caps->flags & (1ULL << 3)) ||
        caps->commit_length != 40 || !caps->commit ||
        std::string(caps->commit, 40) != native_commit ||
        std::memcmp(caps->sha256, expected_sha.data(), 32) != 0) {
        return Status::Invalid("elasticpp capabilities differ from the pinned archive runtime");
    }
    if (!manifest.HasMember("files") || !manifest["files"].IsObject() ||
        manifest["files"].MemberCount() != offsets.size() - 1)
        return Status::Invalid("ESLib sealed file table mismatch");
    std::error_code error;
    std::string root = option("global-index.es-index.native.temp-directory");
    if (root.empty()) root = std::filesystem::temp_directory_path(error).string();
    if (error) return Status::IOError("Cannot locate ESLib temporary directory");
    std::string temporary = root + "/paimon-eslib-XXXXXX";
    if (!mkdtemp(temporary.data()))
        return Status::IOError("Cannot create ESLib temporary directory: ", std::strerror(errno));
    directory_ = temporary;
    for (const auto& [name, offset] : offsets) {
        if (name == kManifest) continue;
        if (name == "write.lock" || (name.size() >= 4 && name.substr(name.size() - 4) == ".tmp") ||
            !manifest["files"].HasMember(name.c_str()))
            return Status::Invalid("Unexpected native ESLib sealed file");
        const auto& file = manifest["files"][name.c_str()];
        if (!file.IsObject() || file.MemberCount() != 2 || !file.HasMember("length") ||
            !file["length"].IsInt64() || file["length"].GetInt64() != offset.length) {
            return Status::Invalid("ESLib sealed file length mismatch");
        }
        PAIMON_ASSIGN_OR_RAISE(std::string expected, Text(file, "sha256"));
        if (!IsHex(expected, 64)) return Status::Invalid("Invalid sealed file digest");
        int32_t fd = ::open((directory_ + "/" + name).c_str(),
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0)
            return Status::IOError("Cannot materialize ESLib sealed file: ", std::strerror(errno));
        ScopeGuard close_file([fd]() { ::close(fd); });
        Digest digest;
        PAIMON_RETURN_NOT_OK(digest.Init());
        for (int64_t copied = 0; copied < offset.length;) {
            int64_t size = std::min<int64_t>(buffer.size(), offset.length - copied);
            PAIMON_RETURN_NOT_OK(
                ReadExact(stream.get(), buffer.data(), size, offset.offset + copied));
            PAIMON_RETURN_NOT_OK(digest.Add(buffer.data(), size));
            for (int64_t written = 0; written < size;) {
                ssize_t result = ::write(fd, buffer.data() + written, size - written);
                if (result < 0 && errno == EINTR) continue;
                if (result <= 0) return Status::IOError("Cannot write native ESLib sealed file");
                written += result;
            }
            copied += size;
        }
        PAIMON_ASSIGN_OR_RAISE(std::string actual, digest.Finish());
        if (expected != actual) return Status::Invalid("ESLib sealed file SHA-256 mismatch");
    }
    abi::Identity identity;
    identity.table = identity_name.data();
    identity.table_length = identity_name.size();
    identity.rows = rows_;
    PAIMON_ASSIGN_OR_RAISE(std::string empty_sha, Sha(""));
    PAIMON_RETURN_NOT_OK(Unhex(empty_sha, identity.partition_sha256));
    PAIMON_RETURN_NOT_OK(Unhex(columns_sha, identity.columns_sha256));
    PAIMON_RETURN_NOT_OK(Unhex(mapping_sha, identity.mapping_sha256));
    abi::OpenRequest request;
    request.path = directory_.data();
    request.path_length = directory_.size();
    request.identity = &identity;
    request.mapping = mapping.data();
    request.mapping_length = mapping.size();
    PAIMON_RETURN_NOT_OK(Unhex(directory_sha, request.directory_sha256));
    std::unique_ptr<abi::OpenResult, decltype(free_open)> opened(open(&request), free_open);
    if (!opened) return Status::IOError("Null elasticpp archive open result");
    if (opened->status.success == 1) handle_ = opened->handle;
    PAIMON_RETURN_NOT_OK(NativeStatus(opened->status));
    if (handle_ == 0 || opened->max_doc != rows_)
        return Status::Invalid("elasticpp archive row span mismatch");
    PAIMON_RETURN_NOT_OK(stream->Close());
    close_stream.Release();
    return Status::OK();
}
Result<std::vector<abi::Hit>> Archive::Search(const std::string& json, int32_t limit,
                                              bool enumerate) const {
    if (limit < 1 || limit > 1000000 || json.size() > 16 * 1024 * 1024) {
        return Status::Invalid("ESLib query exceeds the native V1 result/query window");
    }
    abi::Query query;
    query.json = json.data();
    query.json_length = json.size();
    query.max_rows = limit;
    query.sort_mode = enumerate ? 1 : 0;
    std::unique_ptr<abi::QueryResult, decltype(free_query_)> result(query_(handle_, &query),
                                                                    free_query_);
    if (!result) return Status::IOError("Null elasticpp query result");
    PAIMON_RETURN_NOT_OK(NativeStatus(result->status));
    if (result->complete != 1 || result->timed_out || result->terminated_early ||
        result->approximated || result->relation != 0 || result->total_hits < 0 ||
        result->hit_count < 0 || result->hit_count > limit ||
        (result->hit_count > 0 && !result->hits) ||
        result->hit_count != std::min<int64_t>(result->total_hits, limit) ||
        (enumerate && result->total_hits != result->hit_count)) {
        return Status::IOError("elasticpp returned an incomplete or invalid result");
    }
    std::vector<abi::Hit> hits;
    std::set<int64_t> seen;
    for (int64_t i = 0; i < result->hit_count; ++i) {
        const auto& hit = result->hits[i];
        if (hit.row < 0 || hit.row >= rows_ || !std::isfinite(hit.score) ||
            !seen.insert(hit.row).second) {
            return Status::IOError("elasticpp returned an invalid/duplicate local row id");
        }
        hits.push_back(hit);
    }
    return hits;
}
}  // namespace paimon::eslib
