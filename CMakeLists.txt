cmake_minimum_required(VERSION 3.10)
project(onvif-server-cpp CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB         REQUIRED glib-2.0)
pkg_check_modules(GSTREAMER    REQUIRED gstreamer-1.0)
pkg_check_modules(GST_RTSP_SVR REQUIRED gstreamer-rtsp-server-1.0)

# -----------------------------------------------------------------------------
# onvif-rtsp-server (C++ port of onvif-rtsp-server.py)
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
# onvif-rtsp-watchdog (C++ port of onvif-rtsp-watchdog.py)
# -----------------------------------------------------------------------------
add_executable(onvif-rtsp-watchdog onvif-rtsp-watchdog.cpp)
target_compile_options(onvif-rtsp-watchdog PRIVATE -Wall -Wextra)
install(TARGETS onvif-rtsp-watchdog RUNTIME DESTINATION bin)

# -----------------------------------------------------------------------------
# isp_ctrl (existing C utility — kept as-is for compatibility with the CGIs
# which still shell out to it). Builds as a standalone binary.
# -----------------------------------------------------------------------------
add_executable(isp_ctrl isp_ctrl.c)
target_compile_options(isp_ctrl PRIVATE -Wall)
install(TARGETS isp_ctrl RUNTIME DESTINATION bin)
