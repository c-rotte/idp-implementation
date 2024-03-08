# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

add_library(
    proxygenhttpserver
    RequestHandlerAdaptor.cpp
    SignalHandler.cpp
    HTTPServerAcceptor.cpp
    HTTPServer.cpp
)
target_compile_options(
    proxygenhttpserver
    PRIVATE
        ${_PROXYGEN_COMMON_COMPILE_OPTIONS}
)
if (BUILD_SHARED_LIBS)
    set_property(TARGET proxygenhttpserver PROPERTY POSITION_INDEPENDENT_CODE ON)
    if (DEFINED PACKAGE_VERSION)
        set_target_properties(proxygenhttpserver PROPERTIES VERSION ${PACKAGE_VERSION})
    endif()
endif()

target_link_libraries(
    proxygenhttpserver
    PUBLIC
        proxygen
)
install(
    TARGETS proxygenhttpserver
    EXPORT proxygen-exports
    ARCHIVE DESTINATION ${LIB_INSTALL_DIR}
    LIBRARY DESTINATION ${LIB_INSTALL_DIR}
)

add_library(
        proxygen_masque
        samples/masque/tuntap/TunDevice.cpp
        samples/masque/tuntap/TunManager.cpp
        samples/masque/Capsule.cpp
)
target_compile_options(
        proxygen_masque
        PRIVATE
        ${_PROXYGEN_COMMON_COMPILE_OPTIONS}
)
if (BUILD_SHARED_LIBS)
  set_property(TARGET proxygen_masque PROPERTY POSITION_INDEPENDENT_CODE ON)
  if (DEFINED PACKAGE_VERSION)
    set_target_properties(proxygen_masque PROPERTIES VERSION ${PACKAGE_VERSION})
  endif()
endif()

target_link_libraries(
        proxygen_masque
        PUBLIC
        proxygen
)
install(
        TARGETS proxygen_masque
        EXPORT proxygen-exports
        ARCHIVE DESTINATION ${LIB_INSTALL_DIR}
        LIBRARY DESTINATION ${LIB_INSTALL_DIR}
)

if (BUILD_SAMPLES)
  add_executable(proxygen_push
      samples/push/PushServer.cpp
      samples/push/PushRequestHandler.cpp
  )
  target_compile_options(
      proxygen_push
      PRIVATE
          ${_PROXYGEN_COMMON_COMPILE_OPTIONS}
  )
  target_link_libraries(
      proxygen_push
      PUBLIC
          proxygen
          proxygenhttpserver
  )
  install(
      TARGETS proxygen_push
      EXPORT proxygen-exports
      DESTINATION bin
  )

  add_executable(proxygen_proxy
      samples/proxy/ProxyServer.cpp
      samples/proxy/ProxyHandler.cpp
  )
  target_compile_options(
      proxygen_proxy
      PRIVATE
          ${_PROXYGEN_COMMON_COMPILE_OPTIONS}
  )
  target_link_libraries(
      proxygen_proxy
      PUBLIC
          proxygen
          proxygenhttpserver
  )
  install(
      TARGETS proxygen_proxy
      EXPORT proxygen-exports
      DESTINATION bin
  )

  add_executable(proxygen_static
      samples/static/StaticServer.cpp
      samples/static/StaticHandler.cpp
  )
  target_compile_options(
      proxygen_static
      PRIVATE
          ${_PROXYGEN_COMMON_COMPILE_OPTIONS}
  )
  target_link_libraries(
      proxygen_static
      PUBLIC
          proxygen
          proxygenhttpserver
  )
  install(
      TARGETS proxygen_static
      EXPORT proxygen-exports
      DESTINATION bin
  )

  add_executable(proxygen_echo
      samples/echo/EchoServer.cpp
      samples/echo/EchoHandler.cpp
  )
  target_compile_options(
      proxygen_echo
      PRIVATE
          ${_PROXYGEN_COMMON_COMPILE_OPTIONS}
  )
  target_link_libraries(
      proxygen_echo
      PUBLIC
          proxygen
          proxygenhttpserver
  )
  install(
      TARGETS proxygen_echo
      EXPORT proxygen-exports
      DESTINATION bin
  )

  add_executable(proxygen_masque_server
          samples/masque/MasqueDownstream.cpp
          samples/masque/MasqueUpstream.cpp
          samples/masque/MasqueServer.cpp
          samples/masque/help/SignalHandler.cpp
          samples/masque/help/MasqueUtils.cpp
          samples/hq/FizzContext.cpp
          )
  target_compile_options(
          proxygen_masque_server
          PRIVATE
          ${_PROXYGEN_COMMON_COMPILE_OPTIONS}
          ${_TC_MALLOC_OPTIONS}
  )
  target_link_libraries(
          proxygen_masque_server
          PUBLIC
            proxygen
            proxygen_masque
            proxygenhttpserver
            uring # https://github.com/axboe/liburing
            tuntap # https://github.com/c-rotte/libtuntap
            tins # http://libtins.github.io/
            easy_profiler # https://github.com/yse/easy_profiler
  )
  install(
          TARGETS proxygen_masque_server
          EXPORT proxygen-exports
          DESTINATION bin
  )
endif()

if (BUILD_QUIC AND BUILD_SAMPLES)
  add_library(
      proxygenhqloggerhelper
      samples/hq/HQLoggerHelper.cpp
  )
  target_compile_options(
      proxygenhqloggerhelper
      PRIVATE
          ${_PROXYGEN_COMMON_COMPILE_OPTIONS}
   )
  if (BUILD_SHARED_LIBS)
      set_property(TARGET proxygenhqloggerhelper PROPERTY POSITION_INDEPENDENT_CODE ON)
      if (DEFINED PACKAGE_VERSION)
          set_target_properties(proxygenhqloggerhelper PROPERTIES VERSION ${PACKAGE_VERSION})
      endif()
  endif()

  target_link_libraries(
      proxygenhqloggerhelper
      PUBLIC
          proxygen
  )
  install(
      TARGETS proxygenhqloggerhelper
      EXPORT proxygen-exports
      ARCHIVE DESTINATION ${LIB_INSTALL_DIR}
      LIBRARY DESTINATION ${LIB_INSTALL_DIR}
  )

  add_executable(hq
      samples/hq/main.cpp
      samples/hq/FizzContext.cpp
      samples/hq/H2Server.cpp
      samples/hq/HQClient.cpp
      samples/hq/HQCommandLine.cpp
      samples/hq/HQServer.cpp
      samples/hq/HQServerModule.cpp
      samples/hq/HQParams.cpp
      samples/hq/SampleHandlers.cpp
      samples/masque/help/SignalHandler.cpp
      samples/masque/help/MasqueUtils.cpp
  )
  target_compile_options(
      hq
      PRIVATE
          ${_PROXYGEN_COMMON_COMPILE_OPTIONS}
          ${_TC_MALLOC_OPTIONS}
  )
  target_link_libraries(
      hq
      PUBLIC
          fizz::fizz
          proxygen
          proxygencurl
          proxygenhttpserver
          proxygenhqloggerhelper
          uring # https://github.com/axboe/liburing
          mvfst::mvfst_transport
          mvfst::mvfst_client
          mvfst::mvfst_server
  )
  install(
      TARGETS hq
      EXPORT proxygen-exports
      DESTINATION bin
  )
endif()

file(
    GLOB_RECURSE PROXYGEN_HTTPSERVER_HEADERS_TOINSTALL
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    *.h
)
list(FILTER PROXYGEN_HTTPSERVER_HEADERS_TOINSTALL EXCLUDE REGEX tests/)
list(FILTER PROXYGEN_HTTPSERVER_HEADERS_TOINSTALL EXCLUDE REGEX samples/)
foreach(header ${PROXYGEN_HTTPSERVER_HEADERS_TOINSTALL})
    get_filename_component(header_dir ${header} DIRECTORY)
    install(FILES ${header} DESTINATION include/proxygen/httpserver/${header_dir})
endforeach()

add_subdirectory(tests)
add_subdirectory(filters/tests)
#add_subdirectory(samples/echo/test)
