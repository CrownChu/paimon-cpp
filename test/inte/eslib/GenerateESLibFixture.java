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

import org.apache.paimon.data.BinaryString;
import org.apache.paimon.data.BinaryVector;
import org.apache.paimon.data.GenericRow;
import org.apache.paimon.data.Timestamp;
import org.apache.paimon.fs.local.LocalFileIO;
import org.apache.paimon.fs.PositionOutputStream;
import org.apache.paimon.globalindex.ResultEntry;
import org.apache.paimon.globalindex.io.GlobalIndexFileWriter;
import org.apache.paimon.options.Options;
import org.apache.paimon.types.DataField;
import org.apache.paimon.types.DataTypes;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import org.elasticsearch.eslib.api.model.BuiltinAnalyzer;
import org.elasticsearch.eslib.api.model.FieldIndexConfig;
import java.util.LinkedHashMap;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** Generates actual ESM1 v3 archives with the Paimon Java native writer. */
public final class GenerateESLibFixture {
    public static void main(String[] args) throws Exception {
        Path root = Path.of(args[0]);
        for (String name : List.of("hnsw", "int8_hnsw", "diskbbq", "cosine", "dot_product", "ik")) {
            Path directory = Files.createDirectories(root.resolve(name));
            String metric = name.equals("cosine") || name.equals("dot_product") ? name : "l2";
            String algorithm = name.equals("int8_hnsw") || name.equals("diskbbq") ? name : "hnsw";
            List<DataField> fields = List.of(
                    new DataField(0, "embedding", DataTypes.VECTOR(33, DataTypes.FLOAT())),
                    new DataField(1, "title", DataTypes.STRING()),
                    new DataField(2, "id", DataTypes.BIGINT()),
                    new DataField(3, "tag", DataTypes.STRING()),
                    new DataField(4, "day", DataTypes.DATE()),
                    new DataField(5, "event_time", DataTypes.TIMESTAMP(3)));

            Map<String, String> configuration = new HashMap<>();
            configuration.put("global-index.es-index.backend", "elasticpp");
            configuration.put("global-index.es-index.fields.embedding.algorithm", algorithm);
            configuration.put("global-index.es-index.fields.embedding.metric", metric);
            ESIndexOptions indexOptions = new ESIndexOptions(fields, Options.fromMap(configuration));
            if (name.equals("ik")) {
                // The table-options parser currently restricts IK. Exercise the existing
                // persisted-config factory without changing that Java API in this SDK patch.
                Map<String, FieldIndexConfig> configs = new LinkedHashMap<>(indexOptions.getFieldConfigs());
                configs.put("title", FieldIndexConfig.builder("title", FieldIndexConfig.IndexType.FULLTEXT)
                        .analyzer(BuiltinAnalyzer.IK_MAX_WORD).build());
                indexOptions = ESIndexOptions.fromFieldConfigs(configs, "elasticpp");
            }
            ResultEntry entry;
            try (ESIndexGlobalIndexWriter writer = new ESIndexGlobalIndexWriter(
                    new LocalWriter(directory), fields,
                    indexOptions)) {
                for (int row = 0; row < 128; row++) {
                    writer.write(row, GenericRow.of(
                            row % 7 == 0 ? null : BinaryVector.fromPrimitiveArray(vector(row, metric)),
                            row == 55 ? null : BinaryString.fromString(
                                    (name.equals("ik") ? "中华人民共和国 " : "") + "document " + row + (row % 2 == 0 ? " even" : " odd")),
                            row % 11 == 0 ? null : 9007199254740993L + row,
                            BinaryString.fromString("value_" + row + "*?"),
                            20000 + row, Timestamp.fromEpochMillis(1700000000000L + row)));

                }
                entry = writer.finish().get(0);
            }
            Files.move(directory.resolve(entry.fileName()), directory.resolve("archive.bin"), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
            Files.write(directory.resolve("metadata.bin"), entry.meta());
            if (!ESIndexFileMeta.read(entry.meta()).backend().equals("elasticpp")) throw new AssertionError("wrong backend");
            System.out.println("JAVA_NATIVE_FIXTURE " + name + " rows=128");
        }
    }
    private static float[] vector(int row, String metric) {
        float[] value = new float[33];
        if (metric.equals("cosine") || metric.equals("dot_product")) {
            value[0] = (float) Math.cos(row * 0.01);
            value[1] = (float) Math.sin(row * 0.01);
        } else {
            for (int i = 0; i < value.length; i++) value[i] = row * 0.25f + i * 0.01f;
        }
        return value;
    }
    private static final class LocalWriter implements GlobalIndexFileWriter {
        private final Path directory;
        private final LocalFileIO io = LocalFileIO.create();
        private LocalWriter(Path directory) { this.directory = directory; }
        @Override
        public String newFileName(String prefix) { return prefix + ".index"; }
        @Override
        public PositionOutputStream newOutputStream(String name) throws IOException {
            return io.newOutputStream(new org.apache.paimon.fs.Path(directory.resolve(name).toString()), false);
        }
    }
}
