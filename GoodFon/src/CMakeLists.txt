cmake_minimum_required(VERSION 3.15)
project(GoodFon C)

# По умолчанию — Release (оптимизация + без отладочных символов)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_executable(GoodFon WIN32 main.c resource.rc)

target_link_libraries(GoodFon PRIVATE
    winhttp shlwapi ole32 shell32 user32 advapi32 gdi32 crypt32 uxtheme dwmapi windowscodecs)

target_compile_definitions(GoodFon PRIVATE UNICODE _UNICODE)

if(MSVC)
    # /utf-8 — русские строки в L"" ; /O1 — оптимизация под размер
    target_compile_options(GoodFon PRIVATE /utf-8 /O1 /Gy)
    # /OPT:REF,/OPT:ICF — выбросить неиспользуемый код и слить одинаковые блоки
    target_link_options(GoodFon PRIVATE /OPT:REF /OPT:ICF)
else()
    # GCC/MinGW: оптимизация под размер, стрип, отбрасывание неиспользуемых секций
    target_compile_options(GoodFon PRIVATE -Os -ffunction-sections -fdata-sections)
    target_link_options(GoodFon PRIVATE -s -Wl,--gc-sections)
endif()
