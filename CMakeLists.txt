cmake_minimum_required(VERSION 3.16)
project(billy VERSION 1.04.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(GNUInstallDirs)

find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia Network)
find_package(Qt6 QUIET COMPONENTS DBus)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswresample)

set(BILLY_SOURCES
  src/main.cpp
  src/Settings.cpp
  src/PlaylistModel.cpp
  src/MainWindow.cpp
  src/SingleInstance.cpp
  src/audio/Decoder.cpp
  src/audio/AudioEngine.cpp
  src/widgets/SheepProgress.cpp
  src/widgets/PlaylistView.cpp
  src/widgets/Glyphs.cpp
  src/dialogs/FindDialog.cpp
  src/dialogs/RenameDialog.cpp
  src/dialogs/MultiRenameDialog.cpp
  src/dialogs/DirDialog.cpp
  src/dialogs/FavoritesDialog.cpp
  src/dialogs/SettingsDialog.cpp
  src/dialogs/SleepTimerDialog.cpp
  src/dialogs/InfoDialog.cpp
  src/dialogs/TransferDialog.cpp
)

if(Qt6DBus_FOUND)
  list(APPEND BILLY_SOURCES src/Mpris.cpp)
endif()

add_executable(billy ${BILLY_SOURCES})
target_include_directories(billy PRIVATE src)
target_compile_definitions(billy PRIVATE BILLY_VERSION="${PROJECT_VERSION}")
target_link_libraries(billy PRIVATE
  Qt6::Widgets Qt6::Multimedia Qt6::Network
  PkgConfig::FFMPEG)

if(Qt6DBus_FOUND)
  target_link_libraries(billy PRIVATE Qt6::DBus)
  target_compile_definitions(billy PRIVATE BILLY_HAVE_DBUS)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(billy PRIVATE -Wall -Wextra)
endif()

install(TARGETS billy RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/billy.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/billy.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
