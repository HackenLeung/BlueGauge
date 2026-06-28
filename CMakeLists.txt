cmake_minimum_required(VERSION 3.20)

project(BlueGauge LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(WARNING "This project is intended for x64 builds.")
endif()

add_executable(BlueGauge WIN32
    src/main.cpp
    src/build_info.cpp
    src/app.cpp
    src/tray.cpp
    src/config.cpp
    src/startup.cpp
    src/notify.cpp
    src/logger.cpp
    src/single_instance.cpp
    src/services/bluetooth_device_watcher.cpp
    src/services/scanner_service.cpp
    src/services/update_service.cpp
    src/bluetooth/bluetooth_scanner.cpp
    src/bluetooth/winrt_bluetooth_status.cpp
    src/bluetooth/btc_battery.cpp
    src/bluetooth/ble_battery.cpp
    res/app.rc
)

target_include_directories(BlueGauge PRIVATE src)

target_compile_definitions(BlueGauge PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(BlueGauge PRIVATE
    user32
    shell32
    advapi32
    winhttp
    setupapi
    cfgmgr32
    bthprops
    ole32
    runtimeobject
    uxtheme
)

if(MSVC)
    target_compile_options(BlueGauge PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_compile_options(BlueGauge PRIVATE $<$<CONFIG:Release>:/O2 /GL>)
    target_link_options(BlueGauge PRIVATE $<$<CONFIG:Release>:/OPT:REF /OPT:ICF /LTCG>)
    set_property(TARGET BlueGauge PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
