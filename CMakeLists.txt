cmake_minimum_required(VERSION 4.2)
project(webrtc_app)

set(CMAKE_CXX_STANDARD 20)

find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)

pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0)
pkg_check_modules(GSTREAMER_WEBRTC REQUIRED IMPORTED_TARGET gstreamer-webrtc-1.0)
pkg_check_modules(GSTREAMER_SDP REQUIRED  IMPORTED_TARGET gstreamer-sdp-1.0)

include(FetchContent)

# uWebSockets (header-only C++ wrapper). Shallow clone and only pull the pinned
# uSockets submodule -- a default recursive clone drags in ~2GB (boringssl,
# lsquic, fuzzing corpora, ...) and makes every fresh CMake configure very slow.
FetchContent_Declare(
        uWebSockets
        GIT_REPOSITORY https://github.com/uNetworking/uWebSockets.git
        GIT_TAG        v20.76.0
        GIT_SHALLOW    TRUE
        GIT_SUBMODULES uSockets
        GIT_SUBMODULES_RECURSE FALSE
)

FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG        v1.14.1
        GIT_SHALLOW    TRUE
)

# Add nlohmann/json declaration in your CMakeLists.txt
FetchContent_Declare(
        json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
)

FetchContent_MakeAvailable(uWebSockets spdlog json)

find_package(ZLIB REQUIRED)

# uSockets has no CMakeLists.txt, so build it here (epoll + OpenSSL)
set(USOCKETS_DIR ${uwebsockets_SOURCE_DIR}/uSockets)
add_library(uSockets STATIC
        ${USOCKETS_DIR}/src/bsd.c
        ${USOCKETS_DIR}/src/context.c
        ${USOCKETS_DIR}/src/loop.c
        ${USOCKETS_DIR}/src/socket.c
        ${USOCKETS_DIR}/src/udp.c
        ${USOCKETS_DIR}/src/eventing/epoll_kqueue.c
        ${USOCKETS_DIR}/src/crypto/openssl.c
        ${USOCKETS_DIR}/src/crypto/sni_tree.cpp
)
target_include_directories(uSockets PUBLIC ${USOCKETS_DIR}/src)
target_compile_definitions(uSockets PUBLIC LIBUS_USE_OPENSSL)
target_link_libraries(uSockets PUBLIC OpenSSL::SSL OpenSSL::Crypto)

add_executable(${PROJECT_NAME} ./src/main.cpp
        src/service/WebRTC.cpp
        src/service/WebRTC.h
        src/service/WebRTCSignalingHandler.h
        src/service/WebRTCSignalingHandler.cpp
        src/service/GStreamerMediaService.cpp
        src/service/GStreamerMediaService.h
        src/service/ISignalingSink.h
        src/service/PeerSession.h
        src/service/Room.h
        src/service/PerSocketData.h)

target_include_directories(${PROJECT_NAME} PRIVATE
        ${GSTREAMER_INCLUDE_DIRS}
        ${GSTREAMER_WEBRTC_INCLUDE_DIRS}
        ${GSTREAMER_SDP_INCLUDE_DIRS}
        ${uwebsockets_SOURCE_DIR}/src
)

target_link_libraries(${PROJECT_NAME} PRIVATE
        PkgConfig::GSTREAMER
        PkgConfig::GSTREAMER_WEBRTC
        PkgConfig::GSTREAMER_SDP
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        uSockets
        ZLIB::ZLIB
        OpenSSL::SSL
        OpenSSL::Crypto
        pthread
)
# gst/webrtc is "unstable API" and warns on every include without this
target_compile_definitions(${PROJECT_NAME} PRIVATE GST_USE_UNSTABLE_API)
