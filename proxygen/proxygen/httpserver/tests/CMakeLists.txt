# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

proxygen_add_test(TARGET HTTPServerTests
  SOURCES
    HTTPServerTest.cpp
    RequestHandlerAdaptorTest.cpp
  DEPENDS
    codectestutils
    proxygen
    proxygenhttpserver
    proxygencurl
    proxygen_masque
    testmain
    Boost::filesystem
    Boost::regex
)

set(PROXYGEN_TEST_TARGET_TEMP $PROXYGEN_TEST_TARGET)
set(PROXYGEN_TEST_TARGET "masque_tests")
proxygen_add_test(TARGET MasqueTests
  SOURCES
    MasqueTest.cpp
  DEPENDS
    codectestutils
    proxygen
    proxygenhttpserver
    proxygencurl
    proxygen_masque
)
set(PROXYGEN_TEST_TARGET $PROXYGEN_TEST_TARGET_TEMP)