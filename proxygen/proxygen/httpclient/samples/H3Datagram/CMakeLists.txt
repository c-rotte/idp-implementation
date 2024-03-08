# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

if (BUILD_QUIC)
    add_executable(proxygen_h3datagram_client
            ConnectClient.cpp
            ConnectUDPClient.cpp
            ConnectIPClient.cpp
            H3DatagramClient.cpp
            socket/LayeredMasqueSocket.cpp
            socket/LayeredConnectUDPSocket.cpp
            socket/LayeredConnectIPSocket.cpp
            socket/LayeredSocketGenerator.cpp
            ../../../httpserver/samples/masque/Capsule.cpp
            ../../../httpserver/samples/masque/tuntap/TunDevice.cpp
            ../../../httpserver/samples/masque/tuntap/TunManager.cpp
            ../../../httpserver/samples/masque/help/SignalHandler.cpp
            ../../../httpserver/samples/masque/help/MasqueUtils.cpp)
    target_compile_options(
            proxygen_h3datagram_client PRIVATE
            ${_PROXYGEN_COMMON_COMPILE_OPTIONS}
            ${_TC_MALLOC_OPTIONS}
    )
    target_link_libraries(
            proxygen_h3datagram_client
            PUBLIC
            proxygen
            uring # https://github.com/axboe/liburing
            tuntap # https://github.com/c-rotte/libtuntap
            tins # http://libtins.github.io/
            easy_profiler # https://github.com/yse/easy_profiler
    )
    install(
            TARGETS proxygen_h3datagram_client
            EXPORT proxygen-exports
            DESTINATION bin
    )

    add_executable(proxygen_masque_http_client
            MasqueHttpClient.cpp
            socket/LayeredMasqueSocket.cpp
            socket/LayeredConnectUDPSocket.cpp
            socket/LayeredConnectIPSocket.cpp
            socket/LayeredSocketGenerator.cpp
            ../../../httpserver/samples/masque/Capsule.cpp
            ../../../httpserver/samples/masque/tuntap/TunDevice.cpp
            ../../../httpserver/samples/masque/tuntap/TunManager.cpp
            ../../../httpserver/samples/masque/help/SignalHandler.cpp
            ../../../httpserver/samples/masque/help/MasqueUtils.cpp)
    target_compile_options(
            proxygen_masque_http_client PRIVATE
            ${_PROXYGEN_COMMON_COMPILE_OPTIONS}
            ${_TC_MALLOC_OPTIONS}
    )
    target_link_libraries(
            proxygen_masque_http_client
            PUBLIC
            proxygen
            uring # https://github.com/axboe/liburing
            tuntap # https://github.com/c-rotte/libtuntap
            tins # http://libtins.github.io/
            easy_profiler # https://github.com/yse/easy_profiler
    )
    install(
            TARGETS proxygen_masque_http_client
            EXPORT proxygen-exports
            DESTINATION bin
    )
endif ()
