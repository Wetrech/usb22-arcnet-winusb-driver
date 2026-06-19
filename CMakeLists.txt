cmake_minimum_required(VERSION 3.20)
project(ArcnetDriver C)

add_definitions(-D_WIN32_WINNT=0x0A00)
add_definitions(-UUNICODE -U_UNICODE)
add_definitions(-D_CRT_SECURE_NO_WARNINGS)

# ---------------------------------------------------------
# arcnet: static library  (protocol implementation)
# src/arcnet.c  +  src/arcnet.h  (public header)
# ---------------------------------------------------------
add_library(arcnet STATIC src/arcnet.c)
target_include_directories(arcnet PUBLIC src)
target_link_libraries(arcnet setupapi.lib winusb.lib)

# ---------------------------------------------------------
# test_arcnet: end-to-end test executable
# ---------------------------------------------------------
add_executable(test_arcnet src/test_arcnet.c)
target_link_libraries(test_arcnet arcnet)

# ---------------------------------------------------------
# arcnet_scan: Step 1 - USB device scanner (no WinUSB)
# ---------------------------------------------------------
add_executable(arcnet_scan src/main.c)
target_link_libraries(arcnet_scan setupapi.lib cfgmgr32.lib)

# ---------------------------------------------------------
# winusb_probe: Step 3 - WinUSB endpoint validation
# ---------------------------------------------------------
add_executable(winusb_probe src/winusb_probe.c)
target_link_libraries(winusb_probe setupapi.lib winusb.lib)

# ---------------------------------------------------------
# test_multi: simultaneous two-device open/init diagnostic
# ---------------------------------------------------------
add_executable(test_multi src/test_multi.c)
target_link_libraries(test_multi arcnet)

# ---------------------------------------------------------
# test_ack: ACK detection diagnostic (receiver alive vs dead)
# ---------------------------------------------------------
add_executable(test_ack src/test_ack.c)
target_link_libraries(test_ack arcnet)

# ---------------------------------------------------------
# MSVC compile options
# ---------------------------------------------------------
if(MSVC)
    target_compile_options(arcnet       PRIVATE /W3)
    target_compile_options(test_arcnet  PRIVATE /W3)
    target_compile_options(test_multi   PRIVATE /W3)
    target_compile_options(test_ack     PRIVATE /W3)
    target_compile_options(arcnet_scan  PRIVATE /W3)
    target_compile_options(winusb_probe PRIVATE /W3)
endif()
