cmake_minimum_required(VERSION 3.15)
project(GoodFon C)

add_executable(GoodFon WIN32 main.c resource.rc)

target_link_libraries(GoodFon PRIVATE
    winhttp shlwapi ole32 shell32 user32 advapi32 gdi32 crypt32)

target_compile_definitions(GoodFon PRIVATE UNICODE _UNICODE)

if(MSVC)
    # Исходник в UTF-8 (русские строки в L"" литералах)
    target_compile_options(GoodFon PRIVATE /utf-8 /W3)
endif()
