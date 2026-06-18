cmake_minimum_required(VERSION 3.20)
project(ArcnetDriver C)

# Windows 10/11 API seviyesi
add_definitions(-D_WIN32_WINNT=0x0A00)

# ANSI API kullanıyoruz
add_definitions(-UUNICODE -U_UNICODE)

# MSVC "güvensiz fonksiyon" uyarılarını kapat
add_definitions(-D_CRT_SECURE_NO_WARNINGS)

# ---------------------------------------------------------
# Adım 1: USB cihaz tarayıcı (SetupAPI — WinUSB yok)
# ---------------------------------------------------------
add_executable(arcnet_scan src/main.c)

target_link_libraries(arcnet_scan
    setupapi.lib
    cfgmgr32.lib
)

# ---------------------------------------------------------
# Adım 3: WinUSB probe — cihazı aç, endpoint'leri doğrula
# ---------------------------------------------------------
add_executable(winusb_probe src/winusb_probe.c)

target_link_libraries(winusb_probe
    setupapi.lib
    winusb.lib
)

# ---------------------------------------------------------
# Adım 4: Init + Register komutları
# ---------------------------------------------------------
add_executable(arcnet_io src/arcnet_io.c)

target_link_libraries(arcnet_io
    setupapi.lib
    winusb.lib
)

# ---------------------------------------------------------
# MSVC ortak derleme seçenekleri
# ---------------------------------------------------------
if(MSVC)
    target_compile_options(arcnet_scan  PRIVATE /W3)
    target_compile_options(winusb_probe PRIVATE /W3)
    target_compile_options(arcnet_io    PRIVATE /W3)
endif()
