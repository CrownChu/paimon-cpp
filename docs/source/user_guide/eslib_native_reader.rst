.. Licensed to the Apache Software Foundation (ASF) under one
.. or more contributor license agreements.  See the NOTICE file
.. distributed with this work for additional information
.. regarding copyright ownership.  The ASF licenses this file
.. to you under the Apache License, Version 2.0 (the
.. "License"); you may not use this file except in compliance
.. with the License.  You may obtain a copy of the License at

..   http://www.apache.org/licenses/LICENSE-2.0

.. Unless required by applicable law or agreed to in writing,
.. software distributed under the License is distributed on an
.. "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
.. KIND, either express or implied.  See the License for the
.. specific language governing permissions and limitations
.. under the License.

ESLib Native Index Reader
=========================

The optional ``es-index`` backend reads archives produced by the Paimon Java
ESLib writer with ``global-index.es-index.backend=elasticpp``. It uses elasticpp's
read-only Paimon C ABI v1 directly, without a JVM, JNI or JNA in the SDK process.
The SDK backend is a reader; index construction uses the Java native writer.

Build and link
--------------

Enable the backend explicitly. It is disabled by default and currently supports
Linux x86_64 with an x86-64-v3 CPU and the glibc required by the selected runtime.
OpenSSL Crypto 1.1 or later supplies archive and runtime SHA-256 checks.

.. code-block:: bash

   cmake -S . -B build -DPAIMON_ENABLE_ESLIB=ON -DPAIMON_BUILD_TESTS=ON
   cmake --build build --target paimon_eslib_index_shared paimon-eslib-index-test -j 8

Link the plugin alongside the SDK and the file-system/format plugins used by the
table. As for other factory-registered plugins, retain its constructor registration:

.. code-block:: cmake

   target_link_libraries(reader PRIVATE
       -Wl,--no-as-needed Paimon::paimon_eslib_index_shared -Wl,--as-needed
       Paimon::paimon_shared)

For static linking, retain ``paimon_eslib_index_static`` with whole-archive flags,
and link the SDK's normal static dependencies plus OpenSSL Crypto and libdl.

Runtime options
---------------

Pass these options to ``GlobalIndexScan::Create`` or
``GlobalIndexerFactory::Get("es-index", options)``:

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - Option
     - Meaning
   * - ``global-index.es-index.native.library-path``
     - Absolute path to the immutable ``libelasticpp.so``.
   * - ``global-index.es-index.native.commit``
     - The 40-character native commit recorded in the archive manifest.
   * - ``global-index.es-index.native.sha256``
     - The 64-character digest of that exact shared object.
   * - ``global-index.es-index.native.temp-directory``
     - Optional existing local directory for materialized sealed index files.
       The default is the operating system's temporary directory.

Keep the runtime bundle's dependent libraries beside ``libelasticpp.so`` as its
manifest specifies. The reader verifies the SO and its capability response against
the archive's pinned commit and digest before opening a native reader. Archives
cannot select a library path themselves. The library remains loaded until process
shutdown, while each reader closes its own native handle and deletes its temporary
index directory when destroyed. Each reader materializes one archive; provision
local disk for the total size of concurrently open archives.

For IK fields, set ``ES_PATH_CONF`` to a directory containing
``analysis-ik/main.dic``, ``analysis-ik/quantifier.dic`` and
``analysis-ik/stopword.dic``. Their hashes must reproduce the persisted config
identity, and the selected dictionaries must remain unchanged for the process.
The current Java Paimon table-options parser still rejects IK; the interoperability
test uses its persisted-field-config factory to build the IK fixture. This SDK
change does not change that Java option restriction.

Queries and row IDs
-------------------

Use the normal SDK ``GlobalIndexScan`` API and request index type ``es-index``.
Readers are available for both the primary column and the independently indexed
extra columns of a Java multi-field archive. For example, after creating a scan:

.. code-block:: cpp

   PAIMON_ASSIGN_OR_RAISE(std::shared_ptr<paimon::GlobalIndexReader> reader,
       scan->CreateReader("title", "es-index", std::nullopt));
   if (!reader) {
       return paimon::Status::Invalid("No title es-index exists in this snapshot");
   }
   auto search = std::make_shared<paimon::FullTextSearch>(
       "title", R"({"match":{"query":"paimon lake","operator":"And"}})", 10);
   PAIMON_ASSIGN_OR_RAISE(std::shared_ptr<paimon::ScoredGlobalIndexResult> result,
       reader->VisitFullTextSearch(search));

Supported operations include scalar equality/range/IN/null predicates, exact
keyword comparisons (including generated text keyword companions), string
prefix/suffix/contains/LIKE, HNSW/INT8 HNSW/DiskBBQ vector search, and full-text
match/phrase/boolean queries. Unsupported DSL features return an error instead of
being ignored. Vector runtime options, fuzzy/full-text multi-match/boosting query
forms and SDK index writing are not implemented in this adapter.

Scalar integers retain int64 precision. DATE and TIMESTAMP predicates follow the
Java writer's LONG representation: epoch days and epoch milliseconds respectively.
Vector distance types must match the persisted mapping. L2 scores retain the
native ``1 / (1 + squared_distance)`` value; cosine and normalized dot-product
scores convert to Paimon's similarity with ``2 * score - 1``.

The low-level factory reader returns shard-local row IDs. The SDK scan's existing
offset wrapper adds the shard's global row-range start once and rewrites vector
pre-filters and full-text include-row IDs into the local domain. Scored results
are ordered by row ID with corresponding scores, as the SDK requires. As with
other SDK index backends, the caller merges and ranks candidates across shards.

The current native v1 ABI supports at most one million returned rows per scalar
or full-text request and 10,000 vector neighbors. An incomplete scalar result is
rejected. Callback vector pre-filter enumeration is limited to one million shard
rows. The materialized index is immutable, and searches on one reader are serialized.

Archive compatibility and validation
------------------------------------

The reader requires ESM1 v3 metadata with backend ``elasticpp`` and the accompanying
``.eslib-native-v1.json`` descriptor. It verifies primary fields, physical aliases,
configuration and mapping identities, archive headers/offsets, individual file
lengths and hashes, the sealed directory identity and the native row span.

Java ESM1 v1/v2/legacy Lucene archives require their Java reader. This adapter does
not decode the separate PES1/PEQ1/PQR1 protocol used by the original
``paimon-elasticsearch`` integration. Backend options do not reinterpret an old
archive as native.

Interoperability tests
----------------------

``test/inte/eslib/GenerateESLibFixture.java`` uses the Paimon Java native writer to
produce six small archives: HNSW, INT8 HNSW, DiskBBQ, cosine, dot-product and IK.
Compile it against the native-enabled Paimon ``paimon-eslib`` and its pinned ESLib
API/native/runtime dependencies, then run
``org.apache.paimon.eslib.index.GenerateESLibFixture <fixture-directory>``.

Set ``PAIMON_ESLIB_FIXTURE_DIR``, ``PAIMON_ESLIB_NATIVE_LIBRARY``,
``PAIMON_ESLIB_NATIVE_COMMIT`` and ``PAIMON_ESLIB_NATIVE_SHA256`` to the generated
fixture directory and matching runtime. Set ``ES_PATH_CONF`` for the IK fixture.
Then run:

.. code-block:: bash

   ./build/debug/paimon-eslib-index-test
   cmake --build build --target paimon-eslib-shared-index-test -j 8
   ./build/debug/paimon-eslib-shared-index-test

Both executables run the same reader tests, linking the backend statically and as
a shared plugin respectively. Native fixture tests skip when their environment
variables are absent; a complete interoperability run must report zero skipped
tests. Tests include sparse vectors, nulls, long values above 2^53, metric scores,
pre-filters, global offsets, multi-field discovery through a snapshot/index
manifest, corrupted sealed bytes, metadata/runtime mismatches and cleanup.
