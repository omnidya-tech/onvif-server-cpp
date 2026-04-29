cmake_minimum_required(VERSION 3.10)
project(onvif-server-cpp CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB         REQUIRED glib-2.0)
pkg_check_modules(GSTREAMER    REQUIRED gstreamer-1.0)
pkg_check_modules(GST_RTSP_SVR REQUIRED gstreamer-rtsp-server-1.0)
pkg_check_modules(LIBXML2      REQUIRED libxml-2.0)

# OpenSSL provides MD5 for the recording / replay tokens. libcrypto is the
# library; OpenSSL::Crypto is the modern target.
find_package(OpenSSL REQUIRED)

# -----------------------------------------------------------------------------
# Shared CGI SOAP library (linked into all three CGI binaries)
# -----------------------------------------------------------------------------
add_library(cgi_soap STATIC cgi_soap.cpp)
target_compile_options(cgi_soap PRIVATE -Wall -Wextra)
target_include_directories(cgi_soap PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBXML2_INCLUDE_DIRS}
)
target_link_libraries(cgi_soap PUBLIC ${LIBXML2_LIBRARIES})

# -----------------------------------------------------------------------------
# onvif-rtsp-server (port of onvif-rtsp-server.py)
# -----------------------------------------------------------------------------
add_executable(onvif-rtsp-server onvif-rtsp-server.cpp)
target_compile_options(onvif-rtsp-server PRIVATE -Wall -Wextra)
target_include_directories(onvif-rtsp-server PRIVATE
    ${GLIB_INCLUDE_DIRS}
    ${GSTREAMER_INCLUDE_DIRS}
    ${GST_RTSP_SVR_INCLUDE_DIRS}
)
target_link_libraries(onvif-rtsp-server PRIVATE
    ${GLIB_LIBRARIES}
    ${GSTREAMER_LIBRARIES}
    ${GST_RTSP_SVR_LIBRARIES}
)
install(TARGETS onvif-rtsp-server RUNTIME DESTINATION bin)

# -----------------------------------------------------------------------------
# onvif-rtsp-watchdog (port of onvif-rtsp-watchdog.py)
# -----------------------------------------------------------------------------
add_executable(onvif-rtsp-watchdog onvif-rtsp-watchdog.cpp)
target_compile_options(onvif-rtsp-watchdog PRIVATE -Wall -Wextra)
install(TARGETS onvif-rtsp-watchdog RUNTIME DESTINATION bin)

# -----------------------------------------------------------------------------
# onvif-replay-server (port of onvif-replay-server.py)
# -----------------------------------------------------------------------------
add_executable(onvif-replay-server onvif-replay-server.cpp)
target_compile_options(onvif-replay-server PRIVATE -Wall -Wextra)
target_include_directories(onvif-replay-server PRIVATE
    ${GLIB_INCLUDE_DIRS}
    ${GSTREAMER_INCLUDE_DIRS}
    ${GST_RTSP_SVR_INCLUDE_DIRS}
)
target_link_libraries(onvif-replay-server PRIVATE
    ${GLIB_LIBRARIES}
    ${GSTREAMER_LIBRARIES}
    ${GST_RTSP_SVR_LIBRARIES}
    OpenSSL::Crypto
)
install(TARGETS onvif-replay-server RUNTIME DESTINATION bin)

# -----------------------------------------------------------------------------
# onvif-imaging-cgi (port of onvif-imaging-cgi.py)
# -----------------------------------------------------------------------------
add_executable(onvif-imaging-cgi onvif-imaging-cgi.cpp)
target_compile_options(onvif-imaging-cgi PRIVATE -Wall -Wextra)
target_link_libraries(onvif-imaging-cgi PRIVATE cgi_soap)
install(TARGETS onvif-imaging-cgi RUNTIME DESTINATION bin)

# -----------------------------------------------------------------------------
# onvif-recording-cgi (port of onvif-recording-cgi.py)
# -----------------------------------------------------------------------------
add_executable(onvif-recording-cgi onvif-recording-cgi.cpp)
target_compile_options(onvif-recording-cgi PRIVATE -Wall -Wextra)
target_link_libraries(onvif-recording-cgi PRIVATE
    cgi_soap
    OpenSSL::Crypto
)
install(TARGETS onvif-recording-cgi RUNTIME DESTINATION bin)

# -----------------------------------------------------------------------------
# onvif-analytics-cgi (port of onvif-analytics-cgi.py)
# -----------------------------------------------------------------------------
add_executable(onvif-analytics-cgi onvif-analytics-cgi.cpp)
target_compile_options(onvif-analytics-cgi PRIVATE -Wall -Wextra)
target_link_libraries(onvif-analytics-cgi PRIVATE cgi_soap)
install(TARGETS onvif-analytics-cgi RUNTIME DESTINATION bin)

# -----------------------------------------------------------------------------
# isp_ctrl (existing C utility, kept as-is)
# -----------------------------------------------------------------------------
add_executable(isp_ctrl isp_ctrl.c)
target_compile_options(isp_ctrl PRIVATE -Wall)
install(TARGETS isp_ctrl RUNTIME DESTINATION bin)
