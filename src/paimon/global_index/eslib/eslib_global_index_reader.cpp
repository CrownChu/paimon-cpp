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

#include "paimon/global_index/eslib/eslib_global_index_reader.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

#include "arrow/type.h"
#include "paimon/data/timestamp.h"
#include "paimon/global_index/bitmap_global_index_result.h"
#include "paimon/global_index/bitmap_scored_global_index_result.h"
#include "paimon/predicate/compound_predicate.h"
#include "paimon/predicate/full_text_search.h"
#include "paimon/predicate/vector_search.h"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

namespace paimon::eslib {
namespace {
std::string Quote(const std::string& value) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.String(value.data(), value.size());
    return {buffer.GetString(), buffer.GetSize()};
}
std::string Encode(const rapidjson::Value& value) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    value.Accept(writer);
    return {buffer.GetString(), buffer.GetSize()};
}
std::string List(const std::vector<std::string>& values) {
    std::string result = "[";
    for (const auto& value : values) {
        if (result.size() > 1) result += ',';
        result += value;
    }
    return result + ']';
}
std::string Exists(const std::string& name) {
    return "{\"exists\":{\"field\":" + Quote(name) + "}}";
}
std::string Negate(const std::string& query, const std::string& name) {
    return "{\"bool\":{\"filter\":" + Exists(name) + ",\"must_not\":" + query + "}}";
}
Result<std::string> ScalarValue(const Literal& value, const Field& field) {
    if (value.IsNull()) return Status::Invalid("ESLib scalar comparison requires non-null literal");
    bool keyword = field.type == "KEYWORD" || field.scalar_type == "KEYWORD";
    if (keyword && (value.GetType() == FieldType::STRING || value.GetType() == FieldType::BOOLEAN))
        return Quote(value.ToString());
    if (field.type != "SCALAR")
        return Status::NotImplemented("Unsupported ESLib scalar field type");
    if (value.GetType() == FieldType::BOOLEAN && field.scalar_type == "BOOLEAN")
        return value.GetValue<bool>() ? std::string("true") : std::string("false");
    switch (value.GetType()) {
        case FieldType::TINYINT:
            if (field.scalar_type == "INT" || field.scalar_type == "LONG")
                return std::to_string(value.GetValue<int8_t>());
            break;
        case FieldType::SMALLINT:
        case FieldType::INT:
        case FieldType::BIGINT:
            if (field.scalar_type == "INT" || field.scalar_type == "LONG") return value.ToString();
            break;
        case FieldType::DATE:
            if (field.scalar_type == "LONG") return std::to_string(value.GetValue<int32_t>());
            break;
        case FieldType::TIMESTAMP:
            if (field.scalar_type == "LONG")
                return std::to_string(value.GetValue<Timestamp>().GetMillisecond());
            break;
        case FieldType::FLOAT:
        case FieldType::DOUBLE: {
            double number = value.GetType() == FieldType::FLOAT ? value.GetValue<float>()
                                                                : value.GetValue<double>();
            if (!std::isfinite(number)) return Status::Invalid("Non-finite ESLib literal");
            if (field.scalar_type == "FLOAT" || field.scalar_type == "DOUBLE") {
                rapidjson::Value json(number);
                return Encode(json);
            }
            break;
        }
        default:
            break;
    }
    return Status::Invalid("ESLib literal and persisted field types differ");
}
Result<const Field*> SelectScalar(const Archive& archive, const std::string& name) {
    auto it = archive.Fields().find(name);
    if (it == archive.Fields().end())
        return Status::Invalid("Field absent from ESLib archive: ", name);
    if (it->second.type == "FULLTEXT") {
        it = archive.Fields().find(name + ".keyword");
        if (it == archive.Fields().end())
            return Status::NotImplemented("Text field has no exact keyword companion");
    }
    if (it->second.type == "VECTOR")
        return Status::NotImplemented("Vector scalar comparisons are unsupported");
    return &it->second;
}
Result<std::string> CompareQuery(const Archive& archive, const std::string& name,
                                 const Literal& value, const std::string& operation) {
    PAIMON_ASSIGN_OR_RAISE(const Field* field, SelectScalar(archive, name));
    PAIMON_ASSIGN_OR_RAISE(std::string literal, ScalarValue(value, *field));
    if (operation == "eq" || operation == "ne") {
        std::string query = "{\"term\":{" + Quote(field->physical_name) + ':' + literal + "}}";
        return operation == "ne" ? Negate(query, field->physical_name) : query;
    }
    return "{\"range\":{" + Quote(field->physical_name) + ":{" + Quote(operation) + ':' + literal +
           "}}}";
}
Result<std::string> InQuery(const Archive& archive, const std::string& name,
                            const std::vector<Literal>& values, bool negate) {
    PAIMON_ASSIGN_OR_RAISE(const Field* field, SelectScalar(archive, name));
    std::vector<std::string> literals;
    for (const auto& value : values) {
        // SQL NOT IN with NULL cannot match any row; IN ignores NULL members.
        if (value.IsNull()) {
            if (negate) return std::string("{\"match_none\":{}}");
            continue;
        }
        PAIMON_ASSIGN_OR_RAISE(std::string literal, ScalarValue(value, *field));
        literals.push_back(literal);
    }
    std::string query = "{\"terms\":{" + Quote(field->physical_name) + ':' + List(literals) + "}}";
    return negate ? Negate(query, field->physical_name) : query;
}
Result<std::string> PatternQuery(const Archive& archive, const std::string& name,
                                 const Literal& value, const std::string& operation) {
    PAIMON_ASSIGN_OR_RAISE(const Field* field, SelectScalar(archive, name));
    if (value.IsNull() || value.GetType() != FieldType::STRING ||
        !(field->type == "KEYWORD" || field->scalar_type == "KEYWORD")) {
        return Status::Invalid("ESLib string predicate requires keyword/string");
    }
    std::string input = value.ToString(), pattern;
    if (operation == "like") {
        bool escape = false;
        for (char c : input) {
            if (!escape && c == '\\') {
                escape = true;
                continue;
            }
            if (!escape && (c == '%' || c == '_')) {
                pattern += c == '%' ? '*' : '?';
            } else {
                if (c == '*' || c == '?' || c == '\\') pattern += '\\';
                pattern += c;
            }
            escape = false;
        }
        if (escape) return Status::Invalid("Trailing escape in ESLib LIKE predicate");
    } else {
        for (char c : input) {
            if (c == '*' || c == '?' || c == '\\') pattern += '\\';
            pattern += c;
        }
        if (operation == "suffix" || operation == "contains") pattern.insert(0, "*");
        if (operation == "prefix" || operation == "contains") pattern += '*';
    }
    return "{\"wildcard\":{" + Quote(field->physical_name) + ':' + Quote(pattern) + "}}";
}
Result<std::string> PredicateQuery(const Archive& archive,
                                   const std::shared_ptr<Predicate>& predicate, int32_t depth = 0) {
    if (!predicate || depth > 64) return Status::Invalid("Invalid ESLib vector predicate");
    auto type = predicate->GetFunction().GetType();
    if (auto compound = std::dynamic_pointer_cast<CompoundPredicate>(predicate)) {
        if (type != Function::Type::AND && type != Function::Type::OR)
            return Status::NotImplemented("Unsupported compound predicate");
        std::vector<std::string> children;
        for (const auto& child : compound->Children()) {
            PAIMON_ASSIGN_OR_RAISE(std::string query, PredicateQuery(archive, child, depth + 1));
            children.push_back(query);
        }
        if (children.empty()) return Status::Invalid("Empty compound predicate");
        return type == Function::Type::AND
                   ? "{\"bool\":{\"filter\":" + List(children) + "}}"
                   : "{\"bool\":{\"should\":" + List(children) + ",\"minimum_should_match\":1}}";
    }
    auto leaf = std::dynamic_pointer_cast<LeafPredicate>(predicate);
    if (!leaf) return Status::Invalid("Invalid ESLib leaf predicate");
    const auto& name = leaf->FieldName();
    auto field = archive.Fields().find(name);
    if (field == archive.Fields().end())
        return Status::Invalid("Predicate field is absent from ESLib archive");
    if (type == Function::Type::IS_NULL)
        return "{\"bool\":{\"must_not\":" + Exists(field->second.physical_name) + "}}";
    if (type == Function::Type::IS_NOT_NULL) return Exists(field->second.physical_name);
    if (type == Function::Type::IN || type == Function::Type::NOT_IN)
        return InQuery(archive, name, leaf->Literals(), type == Function::Type::NOT_IN);
    if (leaf->Literals().size() != 1) return Status::Invalid("Invalid predicate literal count");
    const auto& value = leaf->Literals()[0];
    if (value.IsNull()) return std::string("{\"match_none\":{}}");
    switch (type) {
        case Function::Type::EQUAL:
            return CompareQuery(archive, name, value, "eq");
        case Function::Type::NOT_EQUAL:
            return CompareQuery(archive, name, value, "ne");
        case Function::Type::LESS_THAN:
            return CompareQuery(archive, name, value, "lt");
        case Function::Type::LESS_OR_EQUAL:
            return CompareQuery(archive, name, value, "lte");
        case Function::Type::GREATER_THAN:
            return CompareQuery(archive, name, value, "gt");
        case Function::Type::GREATER_OR_EQUAL:
            return CompareQuery(archive, name, value, "gte");
        case Function::Type::STARTS_WITH:
            return PatternQuery(archive, name, value, "prefix");
        case Function::Type::ENDS_WITH:
            return PatternQuery(archive, name, value, "suffix");
        case Function::Type::CONTAINS:
            return PatternQuery(archive, name, value, "contains");
        case Function::Type::LIKE:
            return PatternQuery(archive, name, value, "like");
        default:
            return Status::NotImplemented("Unsupported ESLib predicate");
    }
}
Status CheckObject(const rapidjson::Value& value, const std::set<std::string>& allowed) {
    if (!value.IsObject()) return Status::Invalid("ESLib full-text query must be an object");
    std::set<std::string> seen;
    for (auto it = value.MemberBegin(); it != value.MemberEnd(); ++it) {
        std::string key(it->name.GetString(), it->name.GetStringLength());
        if (!allowed.count(key) || !seen.insert(key).second)
            return Status::Invalid("Unknown/duplicate full-text query member: ", key);
    }
    return Status::OK();
}
Result<std::string> TextQuery(const Archive& archive, const std::string& field_name,
                              const rapidjson::Value& root, int32_t depth = 0) {
    if (depth > 64 || !root.IsObject() || root.MemberCount() != 1)
        return Status::Invalid("Invalid full-text query root");
    std::string type = root.MemberBegin()->name.GetString();
    const auto& value = root.MemberBegin()->value;
    if (type == "boolean") {
        PAIMON_RETURN_NOT_OK(CheckObject(value, {"must", "should", "must_not"}));
        std::string query = "{\"bool\":{";
        bool first = true;
        for (const char* key : {"must", "should", "must_not"}) {
            if (!value.HasMember(key)) continue;
            if (!value[key].IsArray())
                return Status::Invalid("Boolean query clauses must be arrays");
            std::vector<std::string> clauses;
            for (const auto& child : value[key].GetArray()) {
                PAIMON_ASSIGN_OR_RAISE(std::string clause,
                                       TextQuery(archive, field_name, child, depth + 1));
                clauses.push_back(clause);
            }
            if (!first) query += ',';
            first = false;
            query += Quote(key) + ':' + List(clauses);
        }
        if (first) return Status::Invalid("Empty full-text boolean query");
        return query + "}}";
    }
    if (type != "match" && type != "match_phrase" && type != "phrase")
        return Status::NotImplemented("Unsupported ESLib full-text query: ", type);
    PAIMON_RETURN_NOT_OK(
        CheckObject(value, {"query", "terms", "column", "operator", "boost", "fuzziness",
                            "max_expansions", "prefix_length", "slop"}));
    const char* text_key = value.HasMember("query") ? "query" : "terms";
    if (!value.HasMember(text_key) || !value[text_key].IsString() ||
        (value.HasMember("query") && value.HasMember("terms")))
        return Status::Invalid("Missing/ambiguous full-text query string");
    std::string name = field_name;
    if (value.HasMember("column")) {
        if (!value["column"].IsString()) return Status::Invalid("Invalid full-text column");
        name.assign(value["column"].GetString(), value["column"].GetStringLength());
    }
    auto field = archive.Fields().find(name);
    if (field == archive.Fields().end() || field->second.type != "FULLTEXT")
        return Status::Invalid("Full-text column is absent or not indexed as text");
    if (value.HasMember("fuzziness") &&
        (!value["fuzziness"].IsInt() || value["fuzziness"].GetInt() != 0))
        return Status::NotImplemented("ESLib native fuzziness requires separate support");
    if (value.HasMember("max_expansions") || value.HasMember("prefix_length"))
        return Status::NotImplemented("ESLib native fuzzy options are unsupported");
    std::string body = "{\"query\":" + Encode(value[text_key]);
    if (value.HasMember("operator")) {
        if (type != "match" || !value["operator"].IsString())
            return Status::Invalid("Invalid full-text operator");
        std::string operation = value["operator"].GetString();
        std::transform(operation.begin(), operation.end(), operation.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (operation != "and" && operation != "or")
            return Status::Invalid("Full-text operator must be And/Or");
        body += ",\"operator\":" + Quote(operation);
    }
    if (value.HasMember("boost")) {
        if (!value["boost"].IsNumber() || !std::isfinite(value["boost"].GetDouble()) ||
            value["boost"].GetDouble() <= 0)
            return Status::Invalid("Invalid full-text boost");
        body += ",\"boost\":" + Encode(value["boost"]);
    }
    if (value.HasMember("slop")) {
        if (type == "match" || !value["slop"].IsInt() || value["slop"].GetInt() < 0)
            return Status::Invalid("Invalid phrase slop");
        body += ",\"slop\":" + Encode(value["slop"]);
    }
    return '{' + Quote(type == "phrase" ? "match_phrase" : type) + ":{" +
           Quote(field->second.physical_name) + ':' + body + "}}}";
}
std::shared_ptr<ScoredGlobalIndexResult> ToResult(const std::vector<abi::Hit>& hits,
                                                  bool transform_score = false) {
    std::map<int64_t, float> sorted;
    for (const auto& hit : hits)
        sorted.emplace(hit.row, transform_score ? 2 * hit.score - 1 : hit.score);
    RoaringBitmap64 bitmap;
    std::vector<float> scores;
    for (const auto& [row, score] : sorted) {
        bitmap.Add(row);
        scores.push_back(score);
    }
    return std::make_shared<BitmapScoredGlobalIndexResult>(std::move(bitmap), std::move(scores));
}
std::string FilterIds(const std::vector<std::string>& ids) {
    return "{\"terms\":{\"_id\":" + List(ids) + "}}";
}
}  // namespace

Result<std::shared_ptr<GlobalIndexReader>> ESLibGlobalIndexReader::Create(
    const std::shared_ptr<arrow::Field>& schema_field, const GlobalIndexIOMeta& meta,
    const std::shared_ptr<GlobalIndexFileReader>& reader,
    const std::map<std::string, std::string>& options) {
    PAIMON_ASSIGN_OR_RAISE(std::unique_ptr<Archive> archive,
                           Archive::Create(meta, reader, options));
    const std::string& field_name = schema_field->name();
    if (!archive->PrimaryTypes().count(field_name))
        return Status::Invalid("SDK field absent from persisted ESLib primary fields");
    const auto& field = archive->Fields().at(field_name);
    auto type = schema_field->type();
    bool compatible = false;
    if (field.type == "VECTOR") {
        if (type->id() == arrow::Type::FIXED_SIZE_LIST) {
            const auto& list = static_cast<const arrow::FixedSizeListType&>(*type);
            compatible = list.list_size() == field.dimension &&
                         list.value_type()->id() == arrow::Type::FLOAT;
        } else if (type->id() == arrow::Type::LIST) {
            compatible =
                static_cast<const arrow::ListType&>(*type).value_type()->id() == arrow::Type::FLOAT;
        }
    } else if (field.type == "FULLTEXT" || field.type == "KEYWORD" ||
               field.scalar_type == "KEYWORD") {
        compatible = type->id() == arrow::Type::STRING || type->id() == arrow::Type::BOOL;
    } else if (field.type == "SCALAR") {
        compatible = field.scalar_type == "LONG"
                         ? (type->id() == arrow::Type::INT64 || type->id() == arrow::Type::DATE32 ||
                            type->id() == arrow::Type::TIMESTAMP)
                     : field.scalar_type == "INT"
                         ? type->id() == arrow::Type::INT32 || type->id() == arrow::Type::INT16 ||
                               type->id() == arrow::Type::INT8
                     : field.scalar_type == "FLOAT"   ? type->id() == arrow::Type::FLOAT
                     : field.scalar_type == "DOUBLE"  ? type->id() == arrow::Type::DOUBLE
                     : field.scalar_type == "BOOLEAN" ? type->id() == arrow::Type::BOOL
                                                      : false;
    }
    if (!compatible) return Status::Invalid("SDK schema differs from persisted ESLib field type");
    return std::shared_ptr<GlobalIndexReader>(
        new ESLibGlobalIndexReader(std::move(archive), field_name));
}
Result<std::shared_ptr<GlobalIndexResult>> ESLibGlobalIndexReader::Filter(
    const std::string& query) {
    std::lock_guard<std::mutex> lock(mutex_);
    int32_t limit = std::max<int64_t>(1, std::min<int64_t>(1000000, archive_->Rows()));
    std::string source =
        "{\"_source\":false,\"track_total_hits\":true,\"size\":" + std::to_string(limit) +
        ",\"sort\":[\"_doc\"],\"query\":" + query + '}';
    PAIMON_ASSIGN_OR_RAISE(std::vector<abi::Hit> hits, archive_->Search(source, limit, true));
    RoaringBitmap64 bitmap;
    for (const auto& hit : hits) bitmap.Add(hit.row);
    return std::shared_ptr<GlobalIndexResult>(
        std::make_shared<BitmapGlobalIndexResult>([bitmap]() { return bitmap; }));
}
Result<std::shared_ptr<GlobalIndexResult>> ESLibGlobalIndexReader::VisitIsNull() {
    return Filter("{\"bool\":{\"must_not\":" + Exists(archive_->Fields().at(field_).physical_name) +
                  "}}");
}
Result<std::shared_ptr<GlobalIndexResult>> ESLibGlobalIndexReader::VisitIsNotNull() {
    return Filter(Exists(archive_->Fields().at(field_).physical_name));
}
Result<std::shared_ptr<GlobalIndexResult>> ESLibGlobalIndexReader::Compare(
    const Literal& value, const std::string& operation) {
    if (value.IsNull()) return Filter("{\"match_none\":{}}");
    PAIMON_ASSIGN_OR_RAISE(std::string query, CompareQuery(*archive_, field_, value, operation));
    return Filter(query);
}
Result<std::shared_ptr<GlobalIndexResult>> ESLibGlobalIndexReader::VisitEqual(
    const Literal& value) {
    return Compare(value, "eq");
}
Result<std::shared_ptr<GlobalIndexResult>> ESLibGlobalIndexReader::VisitNotEqual(
    const Literal& value) {
    return Compare(value, "ne");
}
Result<std::shared_ptr<GlobalIndexResult>> ESLibGlobalIndexReader::VisitLessThan(
    const Literal& value) {
    return Compare(value, "lt");
}
Result<std::shared_ptr<GlobalIndexResult>> ESLibGlobalIndexReader::VisitLessOrEqual(
    const Literal& value) {
    return Compare(value, "lte");
}
Result<std::shared_ptr<GlobalIndexResult>> ESLibGlobalIndexReader::VisitGreaterThan(
    const Literal& value) {
    return Compare(value, "gt");
}
Result<std::shared_ptr<GlobalIndexResult>> ESLibGlobalIndexReader::VisitGreaterOrEqual(
    const Literal& value) {
    return Compare(value, "gte");
}
Result<std::shared_ptr<GlobalIndexResult>> ESLibGlobalIndexReader::In(
    const std::vector<Literal>& values, bool negate) {
    PAIMON_ASSIGN_OR_RAISE(std::string query, InQuery(*archive_, field_, values, negate));
    return Filter(query);
}
Result<std::shared_ptr<GlobalIndexResult>> ESLibGlobalIndexReader::VisitIn(
    const std::vector<Literal>& values) {
    return In(values, false);
}
Result<std::shared_ptr<GlobalIndexResult>> ESLibGlobalIndexReader::VisitNotIn(
    const std::vector<Literal>& values) {
    return In(values, true);
}
Result<std::shared_ptr<GlobalIndexResult>> ESLibGlobalIndexReader::Pattern(
    const Literal& value, const std::string& operation) {
    PAIMON_ASSIGN_OR_RAISE(std::string query, PatternQuery(*archive_, field_, value, operation));
    return Filter(query);
}
Result<std::shared_ptr<GlobalIndexResult>> ESLibGlobalIndexReader::VisitStartsWith(
    const Literal& value) {
    return Pattern(value, "prefix");
}
Result<std::shared_ptr<GlobalIndexResult>> ESLibGlobalIndexReader::VisitEndsWith(
    const Literal& value) {
    return Pattern(value, "suffix");
}
Result<std::shared_ptr<GlobalIndexResult>> ESLibGlobalIndexReader::VisitContains(
    const Literal& value) {
    return Pattern(value, "contains");
}
Result<std::shared_ptr<GlobalIndexResult>> ESLibGlobalIndexReader::VisitLike(const Literal& value) {
    return Pattern(value, "like");
}

Result<std::shared_ptr<ScoredGlobalIndexResult>> ESLibGlobalIndexReader::VisitVectorSearch(
    const std::shared_ptr<VectorSearch>& search) {
    if (!search || search->field_name != field_ || search->limit < 1 || search->limit > 10000)
        return Status::Invalid("Invalid ESLib vector search field/limit");
    const auto& field = archive_->Fields().at(field_);
    if (field.type != "VECTOR" || search->query.size() != static_cast<size_t>(field.dimension))
        return Status::Invalid("ESLib vector dimension/type mismatch");
    VectorSearch::DistanceType metric = field.metric == "cosine"
                                            ? VectorSearch::DistanceType::COSINE
                                        : field.metric == "ip" || field.metric == "dot_product"
                                            ? VectorSearch::DistanceType::INNER_PRODUCT
                                            : VectorSearch::DistanceType::EUCLIDEAN;
    if (search->distance_type && *search->distance_type != metric)
        return Status::Invalid("ESLib vector distance type mismatch");
    if (!search->options.empty())
        return Status::NotImplemented("ESLib native runtime vector options are not yet supported");
    double squared = 0;
    std::vector<std::string> vector;
    for (float value : search->query) {
        if (!std::isfinite(value)) return Status::Invalid("Non-finite ESLib query vector");
        squared += static_cast<double>(value) * value;
        rapidjson::Value number{static_cast<double>(value)};
        vector.push_back(Encode(number));
    }
    if ((metric == VectorSearch::DistanceType::COSINE && squared == 0) ||
        (metric == VectorSearch::DistanceType::INNER_PRODUCT && std::abs(squared - 1) >= 1e-3))
        return Status::Invalid("ESLib cosine/dot-product query vector norm is invalid");
    std::vector<std::string> filters;
    if (search->predicate) {
        PAIMON_ASSIGN_OR_RAISE(std::string predicate, PredicateQuery(*archive_, search->predicate));
        filters.push_back(predicate);
    }
    if (search->pre_filter) {
        if (archive_->Rows() > 1000000)
            return Status::NotImplemented(
                "Native V1 prefilter enumeration exceeds one million rows");
        std::vector<std::string> ids;
        for (int64_t row = 0; row < archive_->Rows(); ++row)
            if (search->pre_filter(row))
                ids.push_back(Quote(std::to_string(archive_->RowStart() + row)));
        if (ids.empty()) return ToResult({});
        filters.push_back(FilterIds(ids));
    }
    int32_t candidates = std::min(10000, std::max(search->limit, 4 * search->limit));
    std::string knn = "{\"field\":" + Quote(field.physical_name) +
                      ",\"query_vector\":" + List(vector) +
                      ",\"k\":" + std::to_string(search->limit) +
                      ",\"num_candidates\":" + std::to_string(candidates);
    if (!filters.empty()) knn += ",\"filter\":{\"bool\":{\"filter\":" + List(filters) + "}}";
    knn += '}';
    std::string source =
        "{\"_source\":false,\"track_total_hits\":true,\"size\":" + std::to_string(search->limit) +
        ",\"knn\":" + knn + '}';
    std::lock_guard<std::mutex> lock(mutex_);
    PAIMON_ASSIGN_OR_RAISE(std::vector<abi::Hit> hits,
                           archive_->Search(source, search->limit, false));
    return ToResult(hits, metric != VectorSearch::DistanceType::EUCLIDEAN);
}
Result<std::shared_ptr<ScoredGlobalIndexResult>> ESLibGlobalIndexReader::VisitFullTextSearch(
    const std::shared_ptr<FullTextSearch>& search) {
    if (!search || search->field_name != field_ || search->limit < 1 || search->limit > 1000000 ||
        search->query.size() > 16 * 1024 * 1024)
        return Status::Invalid("Invalid ESLib full-text search");
    std::string text_field = field_;
    if (archive_->Fields().at(text_field).type == "KEYWORD") text_field += ".fulltext";
    rapidjson::Document root;
    root.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag>(
        search->query.data(), search->query.size());
    if (root.HasParseError()) return Status::Invalid("Invalid full-text JSON");
    PAIMON_ASSIGN_OR_RAISE(std::string query, TextQuery(*archive_, text_field, root));
    if (search->include_row_ids) {
        std::vector<std::string> ids;
        for (auto it = search->include_row_ids->Begin(); it != search->include_row_ids->End();
             ++it) {
            if (*it >= 0 && *it < archive_->Rows())
                ids.push_back(Quote(std::to_string(archive_->RowStart() + *it)));
            if (ids.size() > 1000000)
                return Status::Invalid("ESLib full-text include-row-id limit exceeded");
        }
        if (ids.empty()) return ToResult({});
        query = "{\"bool\":{\"must\":" + query + ",\"filter\":" + FilterIds(ids) + "}}";
    }
    std::string source =
        "{\"_source\":false,\"track_total_hits\":true,\"size\":" + std::to_string(search->limit) +
        ",\"query\":" + query + '}';
    std::lock_guard<std::mutex> lock(mutex_);
    PAIMON_ASSIGN_OR_RAISE(std::vector<abi::Hit> hits,
                           archive_->Search(source, search->limit, false));
    return ToResult(hits);
}
}  // namespace paimon::eslib
