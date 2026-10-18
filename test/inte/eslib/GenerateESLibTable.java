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

package org.apache.paimon.eslib.index;

import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import org.apache.paimon.Snapshot;
import org.apache.paimon.catalog.Catalog;
import org.apache.paimon.catalog.CatalogContext;
import org.apache.paimon.catalog.CatalogFactory;
import org.apache.paimon.catalog.Identifier;
import org.apache.paimon.data.BinaryString;
import org.apache.paimon.data.BinaryVector;
import org.apache.paimon.data.GenericRow;
import org.apache.paimon.data.InternalArray;
import org.apache.paimon.data.InternalRow;
import org.apache.paimon.data.Timestamp;
import org.apache.paimon.globalindex.GlobalIndexBuilderUtils;
import org.apache.paimon.globalindex.GlobalIndexMultiColumnWriter;
import org.apache.paimon.globalindex.ResultEntry;
import org.apache.paimon.index.IndexFileMeta;
import org.apache.paimon.io.CompactIncrement;
import org.apache.paimon.io.DataIncrement;
import org.apache.paimon.reader.RecordReader;
import org.apache.paimon.schema.Schema;
import org.apache.paimon.table.FileStoreTable;
import org.apache.paimon.table.SpecialFields;
import org.apache.paimon.table.sink.BatchTableCommit;
import org.apache.paimon.table.sink.BatchTableWrite;
import org.apache.paimon.table.sink.BatchWriteBuilder;
import org.apache.paimon.table.sink.CommitMessage;
import org.apache.paimon.table.sink.CommitMessageImpl;
import org.apache.paimon.table.source.DataSplit;
import org.apache.paimon.table.source.ReadBuilder;
import org.apache.paimon.table.source.Split;
import org.apache.paimon.types.DataField;
import org.apache.paimon.types.DataTypes;
import org.apache.paimon.types.RowType;
import org.apache.paimon.types.VectorType;
import org.apache.paimon.utils.CloseableIterator;
import org.apache.paimon.utils.Range;

/** Writes real committed Paimon tables and Java full-scan data oracles for the SDK. */
public final class GenerateESLibTable {
    private static final int DIMENSIONS = 33;

    public static void main(String[] args) throws Exception {
        Path root = Path.of(args[0]);
        if (Files.exists(root))
            throw new IllegalArgumentException("Fixture root must be new");
        Files.createDirectories(root);
        org.apache.paimon.fs.Path warehouse =
                new org.apache.paimon.fs.Path(root.resolve("warehouse").toString());
        try (Catalog catalog = CatalogFactory.createCatalog(CatalogContext.create(warehouse))) {
            catalog.createDatabase("sdk", true);
            for (String format : Arrays.asList("parquet", "orc")) {
                for (String algorithm : Arrays.asList("hnsw", "int8_hnsw", "diskbbq")) {
                    String name = format + "_" + algorithm;
                    Identifier identifier = Identifier.create("sdk", name);
                    Schema.Builder schema =
                            Schema.newBuilder()
                                    .column("embedding",
                                            format.equals("orc")
                                                    ? DataTypes.ARRAY(DataTypes.FLOAT())
                                                    : DataTypes.VECTOR(
                                                              DIMENSIONS, DataTypes.FLOAT()))
                                    .column("title", DataTypes.STRING())
                                    .column("id", DataTypes.BIGINT())
                                    .column("tag", DataTypes.STRING())
                                    .column("day", DataTypes.DATE())
                                    .column("event_time", DataTypes.TIMESTAMP(3))
                                    .column("part", DataTypes.STRING())
                                    .partitionKeys("part")
                                    .option("bucket", "-1")
                                    .option("file.format", format)
                                    .option("row-tracking.enabled", "true")
                                    .option("data-evolution.enabled", "true")
                                    .option("compaction.min.file-num", "100")
                                    .option("snapshot.num-retained.min", "20")
                                    .option("global-index.es-index.backend", "elasticpp")
                                    .option("global-index.es-index.fields.embedding.algorithm",
                                            algorithm)
                                    .option("global-index.es-index.fields.embedding.metric", "l2")
                                    .option("global-index.es-index.fields.embedding.dimension",
                                            String.valueOf(DIMENSIONS));
                    catalog.createTable(identifier, schema.build(), false);
                    FileStoreTable table = (FileStoreTable) catalog.getTable(identifier);
                    append(table, 0, 64);
                    table = (FileStoreTable) catalog.getTable(identifier);
                    append(table, 64, 128);
                    table = (FileStoreTable) catalog.getTable(identifier);
                    buildIndexes(table);
                    Snapshot indexed = table.snapshotManager().latestSnapshot();
                    List<Map<String, Object>> indexedRows = fullScan(table, indexed);
                    append(table, 128, 136);
                    table = (FileStoreTable) catalog.getTable(identifier);
                    Snapshot latest = table.snapshotManager().latestSnapshot();
                    List<Map<String, Object>> latestRows = fullScan(table, latest);
                    if (indexedRows.size() != 128 || latestRows.size() != 136) {
                        throw new AssertionError("Java committed table row counts differ");
                    }
                    Map<String, Object> oracle = new LinkedHashMap<>();
                    oracle.put("tablePath", "warehouse/sdk.db/" + name);
                    oracle.put("indexedSnapshot", indexed.id());
                    oracle.put("latestSnapshot", latest.id());
                    oracle.put("indexedRows", indexedRows);
                    oracle.put("latestRows", latestRows);
                    Path output =
                            Files.createDirectories(root.resolve(name)).resolve("oracle.json");
                    new ObjectMapper().writerWithDefaultPrettyPrinter().writeValue(
                            output.toFile(), oracle);
                    System.out.println("JAVA_COMMITTED_TABLE " + name + " indexedSnapshot="
                            + indexed.id() + " latestSnapshot=" + latest.id() + " rows=128/136");
                }
            }
        }
    }

    private static void append(FileStoreTable table, int from, int to) throws Exception {
        BatchWriteBuilder builder = table.newBatchWriteBuilder();
        try (BatchTableWrite write = builder.newWrite();
                BatchTableCommit commit = builder.newCommit()) {
            for (int row = from; row < to; row++) {
                float[] vector = new float[DIMENSIONS];
                for (int i = 0; i < vector.length; i++) vector[i] = row * 0.25f + i * 0.01f;
                write.write(
                        GenericRow.of(row % 7 == 0 ? null : BinaryVector.fromPrimitiveArray(vector),
                                row == 55 ? null
                                          : BinaryString.fromString("document " + row
                                                    + (row % 2 == 0 ? " even" : " odd")),
                                row % 11 == 0 ? null : 9007199254740993L + row,
                                BinaryString.fromString("value_" + row + "*?"), 20000 + row,
                                Timestamp.fromEpochMillis(1700000000000L + row),
                                BinaryString.fromString(row < 64 ? "p0"
                                                : row < 128      ? "p1"
                                                                 : "p2")));
            }
            commit.commit(write.prepareCommit());
        }
    }

    private static void buildIndexes(FileStoreTable table) throws Exception {
        List<DataField> fields = table.rowType().getFields().subList(0, 6);
        RowType readType = SpecialFields.rowTypeWithRowId(table.rowType());
        int rowIdPosition = readType.getFieldIndex(SpecialFields.ROW_ID.name());
        List<CommitMessage> messages = new ArrayList<>();
        for (Split split : table.newReadBuilder().newScan().plan().splits()) {
            DataSplit data = (DataSplit) split;
            Range range = GlobalIndexBuilderUtils.calcRowRange(data);
            for (long start = range.from; start <= range.to; start += 32) {
                Range shard = new Range(start, Math.min(start + 31, range.to));
                try (ESIndexGlobalIndexWriter writer = (ESIndexGlobalIndexWriter)
                                GlobalIndexBuilderUtils.createIndexWriter(table, "es-index",
                                        fields.get(0), fields.subList(1, fields.size()),
                                        table.coreOptions().toConfiguration(), data.partition(),
                                        data.bucket(), shard, null)) {
                    GlobalIndexMultiColumnWriter multi = (GlobalIndexMultiColumnWriter) writer;
                    try (RecordReader<InternalRow> reader = table.newReadBuilder()
                                    .withReadType(readType)
                                    .newRead()
                                    .createReader(Collections.singletonList(split));
                            CloseableIterator<InternalRow> rows = reader.toCloseableIterator()) {
                        while (rows.hasNext()) {
                            InternalRow row = rows.next();
                            long rowId = row.getLong(rowIdPosition);
                            if (rowId >= shard.from && rowId <= shard.to)
                                multi.write(rowId - shard.from, row);
                        }
                    }
                    List<ResultEntry> entries = writer.finish();
                    List<IndexFileMeta> files = GlobalIndexBuilderUtils.toIndexFileMetas(
                            table.fileIO(), table.store().pathFactory().globalIndexFileFactory(),
                            table.coreOptions(), shard, fields, "es-index", entries, null);
                    messages.add(new CommitMessageImpl(data.partition(), data.bucket(), null,
                            DataIncrement.indexIncrement(files),
                            CompactIncrement.emptyIncrement()));
                }
            }
        }
        if (messages.size() != 4)
            throw new AssertionError("Expected four native index shards");
        try (BatchTableCommit commit = table.newBatchWriteBuilder().newCommit()) {
            commit.commit(messages);
        }
    }

    private static List<Map<String, Object>> fullScan(FileStoreTable table, Snapshot snapshot)
            throws Exception {
        RowType type = SpecialFields.rowTypeWithRowId(table.rowType());
        ReadBuilder builder = table.newReadBuilder().withSnapshot(snapshot).withReadType(type);
        List<Map<String, Object>> result = new ArrayList<>();
        try (RecordReader<InternalRow> reader =
                        builder.newRead().createReader(builder.newScan().plan());
                CloseableIterator<InternalRow> rows = reader.toCloseableIterator()) {
            while (rows.hasNext()) {
                InternalRow row = rows.next();
                Map<String, Object> values = new LinkedHashMap<>();
                values.put("rowId", row.getLong(type.getFieldIndex(SpecialFields.ROW_ID.name())));
                List<Float> vector = null;
                if (!row.isNullAt(0)) {
                    vector = new ArrayList<>();
                    InternalArray source = table.rowType().getTypeAt(0) instanceof VectorType
                            ? row.getVector(0)
                            : row.getArray(0);
                    for (int i = 0; i < source.size(); i++) vector.add(source.getFloat(i));
                }
                values.put("embedding", vector);
                values.put("title", row.isNullAt(1) ? null : row.getString(1).toString());
                values.put("id", row.isNullAt(2) ? null : row.getLong(2));
                values.put("tag", row.getString(3).toString());
                values.put("day", row.getInt(4));
                values.put("event_time", row.getTimestamp(5, 3).getMillisecond());
                values.put("part", row.getString(6).toString());
                result.add(values);
            }
        }
        result.sort(Comparator.comparingLong(row -> (Long) row.get("rowId")));
        return result;
    }
}
