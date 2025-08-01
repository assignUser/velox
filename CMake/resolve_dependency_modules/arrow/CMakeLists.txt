# Copyright (c) Facebook, Inc. and its affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
project(Arrow)

if(VELOX_ENABLE_ARROW)
  if(Thrift_FOUND)
    set(THRIFT_SOURCE "SYSTEM")
  else()
    set(THRIFT_SOURCE "BUNDLED")
  endif()

  set(ARROW_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/arrow_ep")
  set(ARROW_CMAKE_ARGS
      -DARROW_PARQUET=OFF
      -DARROW_DEPENDENCY_SOURCE=AUTO
      -DARROW_WITH_THRIFT=ON
      -DARROW_WITH_LZ4=ON
      -DARROW_WITH_SNAPPY=ON
      -DARROW_WITH_ZLIB=ON
      -DARROW_WITH_ZSTD=ON
      -DARROW_JEMALLOC=OFF
      -DARROW_SIMD_LEVEL=NONE
      -DARROW_RUNTIME_SIMD_LEVEL=NONE
      -DARROW_WITH_UTF8PROC=OFF
      -DARROW_TESTING=ON
      -DCMAKE_INSTALL_PREFIX=${ARROW_PREFIX}/install
      -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
      -DARROW_BUILD_STATIC=ON
      -DThrift_SOURCE=${THRIFT_SOURCE}
      -DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
      # Remove with Arrow upgrade to Arrow 20.
      -DARROW_CXXFLAGS=-Wno-documentation)
  set(ARROW_LIBDIR ${ARROW_PREFIX}/install/${CMAKE_INSTALL_LIBDIR})

  add_library(thrift STATIC IMPORTED GLOBAL)
  if(NOT Thrift_FOUND)
    set(THRIFT_ROOT ${ARROW_PREFIX}/src/arrow_ep-build/thrift_ep-install)
    set(THRIFT_LIB ${THRIFT_ROOT}/lib/libthrift.a)

    file(MAKE_DIRECTORY ${THRIFT_ROOT}/include)
    set(THRIFT_INCLUDE_DIR ${THRIFT_ROOT}/include)
  endif()

  set_property(TARGET thrift PROPERTY INTERFACE_INCLUDE_DIRECTORIES
                                      ${THRIFT_INCLUDE_DIR})
  set_property(TARGET thrift PROPERTY IMPORTED_LOCATION ${THRIFT_LIB})

  set(VELOX_ARROW_BUILD_VERSION 15.0.0)
  set(VELOX_ARROW_BUILD_SHA256_CHECKSUM
      ab74c60c46938505c8cd7599b1d2826c68450645d5860d0ff40f67e371a5d0b5)
  set(VELOX_ARROW_SOURCE_URL
      "https://github.com/apache/arrow/archive/refs/tags/apache-arrow-${VELOX_ARROW_BUILD_VERSION}.tar.gz"
  )

  velox_resolve_dependency_url(ARROW)

  ExternalProject_Add(
    arrow_ep
    PREFIX ${ARROW_PREFIX}
    URL ${VELOX_ARROW_SOURCE_URL}
    URL_HASH ${VELOX_ARROW_BUILD_SHA256_CHECKSUM}
    SOURCE_SUBDIR cpp
    CMAKE_ARGS ${ARROW_CMAKE_ARGS}
    BUILD_BYPRODUCTS ${ARROW_LIBDIR}/libarrow.a
                     ${ARROW_LIBDIR}/libarrow_testing.a ${THRIFT_LIB}
    PATCH_COMMAND git apply ${CMAKE_CURRENT_LIST_DIR}/thrift-download.patch &&
                  git apply ${CMAKE_CURRENT_LIST_DIR}/cmake-compatibility.patch)

  add_library(arrow STATIC IMPORTED GLOBAL)
  add_library(arrow_testing STATIC IMPORTED GLOBAL)
  add_dependencies(arrow arrow_ep)
  add_dependencies(arrow_testing arrow)
  file(MAKE_DIRECTORY ${ARROW_PREFIX}/install/include)
  set_target_properties(
    arrow arrow_testing PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                   ${ARROW_PREFIX}/install/include)
  set_target_properties(arrow PROPERTIES IMPORTED_LOCATION
                                         ${ARROW_LIBDIR}/libarrow.a)
  set_property(TARGET arrow PROPERTY INTERFACE_LINK_LIBRARIES ${RE2} thrift)
  set_target_properties(
    arrow_testing PROPERTIES IMPORTED_LOCATION
                             ${ARROW_LIBDIR}/libarrow_testing.a)

endif()
