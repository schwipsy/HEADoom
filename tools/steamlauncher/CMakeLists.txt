# Headcutter Steam launcher (MIT). Separate project on purpose: it must never
# be added to the engine's CMake tree, so the GPL engine never links Steam.
cmake_minimum_required(VERSION 3.16)
project(HeadcutterSteamLauncher CXX)

set(STEAMWORKS_SDK "S:/Z-GameDev/GzdoomGame/STEAM/steamworks/sdk" CACHE PATH "Steamworks SDK root (the folder with public/ and redistributable_bin/)")
set(HC_STEAM_APPID 0 CACHE STRING "Steam App ID; nonzero makes the launcher relaunch through Steam")
option(HC_ALLOW_CLEAR "Allow CLRACH (dev builds only)" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(HeadcutterSteam WIN32 steamlauncher.cpp)
target_include_directories(HeadcutterSteam PRIVATE "${STEAMWORKS_SDK}/public")
target_link_libraries(HeadcutterSteam PRIVATE "${STEAMWORKS_SDK}/redistributable_bin/win64/steam_api64.lib" ws2_32 winmm)
target_compile_definitions(HeadcutterSteam PRIVATE HC_STEAM_APPID=${HC_STEAM_APPID} UNICODE _UNICODE)
if(HC_ALLOW_CLEAR)
	target_compile_definitions(HeadcutterSteam PRIVATE HC_ALLOW_CLEAR)
endif()
add_custom_command(TARGET HeadcutterSteam POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
	"${STEAMWORKS_SDK}/redistributable_bin/win64/steam_api64.dll" "$<TARGET_FILE_DIR:HeadcutterSteam>")

add_executable(pipetest pipetest.cpp)
