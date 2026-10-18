# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES
                                             "^(x86_64|amd64)$")
    message(FATAL_ERROR "The current ESLib native runtime requires Linux x86_64")
endif()

add_paimon_lib(paimon_eslib_index
               SOURCES
               eslib_archive.cpp
               eslib_global_index.cpp
               eslib_global_index_reader.cpp
               DEPENDENCIES
               paimon_shared
               OpenSSL::Crypto
               STATIC_LINK_LIBS
               arrow
               fmt
               Threads::Threads
               OpenSSL::Crypto
               ${CMAKE_DL_LIBS}
               SHARED_LINK_LIBS
               paimon_shared
               OpenSSL::Crypto
               ${CMAKE_DL_LIBS}
               SHARED_LINK_FLAGS
               ${PAIMON_VERSION_SCRIPT_FLAGS})
target_link_libraries(paimon_eslib_index_objlib PRIVATE OpenSSL::Crypto)

if(PAIMON_BUILD_TESTS)
    paimon_link_libraries_whole_archive(PAIMON_ESLIB_INDEX_STATIC_LINK_LIBS
                                        paimon_eslib_index_static)
    add_paimon_test(eslib_index_test
                    SOURCES
                    eslib_global_index_test.cpp
                    eslib_table_interop_test.cpp
                    STATIC_LINK_LIBS
                    paimon_shared
                    test_utils_static
                    ${PAIMON_ESLIB_INDEX_STATIC_LINK_LIBS}
                    ${PAIMON_LOCAL_FILE_SYSTEM_STATIC_LINK_LIBS}
                    ${PAIMON_PARQUET_FILE_FORMAT_STATIC_LINK_LIBS}
                    ${PAIMON_AVRO_FILE_FORMAT_STATIC_LINK_LIBS}
                    ${PAIMON_ORC_FILE_FORMAT_STATIC_LINK_LIBS}
                    parquet
                    OpenSSL::Crypto
                    ${CMAKE_DL_LIBS}
                    ${GTEST_LINK_TOOLCHAIN})
    if(PAIMON_BUILD_SHARED)
        paimon_link_libraries_no_as_needed(PAIMON_ESLIB_SHARED_TEST_LIBS
                                           paimon_eslib_index_shared)
        add_paimon_test(eslib_shared_index_test
                        SOURCES
                        eslib_global_index_test.cpp
                        eslib_table_interop_test.cpp
                        STATIC_LINK_LIBS
                        paimon_shared
                        test_utils_static
                        ${PAIMON_ESLIB_SHARED_TEST_LIBS}
                        ${PAIMON_LOCAL_FILE_SYSTEM_STATIC_LINK_LIBS}
                        ${PAIMON_PARQUET_FILE_FORMAT_STATIC_LINK_LIBS}
                        ${PAIMON_AVRO_FILE_FORMAT_STATIC_LINK_LIBS}
                        ${PAIMON_ORC_FILE_FORMAT_STATIC_LINK_LIBS}
                        parquet
                        ${GTEST_LINK_TOOLCHAIN})
    endif()
endif()
