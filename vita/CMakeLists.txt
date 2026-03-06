cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR
      "VITASDK is not set. Install the Vita SDK and set VITASDK to its path, e.g.:\n"
      "  export VITASDK=/usr/local/vitasdk\n"
      "See vita/README.md for install instructions (vdpm / vitasdk).")
  endif()
endif()

if(NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
  message(FATAL_ERROR
    "Vita SDK toolchain not found:\n  ${CMAKE_TOOLCHAIN_FILE}\n"
    "Install the Vita SDK (see https://vitasdk.org/ or vita/README.md) and set VITASDK to the SDK path, e.g.:\n"
    "  export VITASDK=/usr/local/vitasdk")
endif()

project(snapcast_vita C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Snapcast")
set(VITA_TITLEID  "SNAP00001")
set(VITA_VERSION  "01.00")
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -O2 -fno-lto")

add_executable(${PROJECT_NAME}
  src/main.c
  src/json.c
  src/config.c
  src/network.c
  src/audio.c
  src/gui.c
  src/time_sync.c
)

target_include_directories(${PROJECT_NAME} PRIVATE
  src
  ${VITASDK}/arm-vita-eabi/include
)

target_link_libraries(${PROJECT_NAME}
  vita2d
  SceDisplay_stub
  SceGxm_stub
  SceCtrl_stub
  ScePgf_stub
  ScePvf_stub
  SceAudio_stub
  SceNet_stub
  SceNetCtl_stub
  SceKernelThreadMgr_stub
  SceSysmodule_stub
  SceCommonDialog_stub
  SceIme_stub
  ScePower_stub
  SceTouch_stub
  SceRtc_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceKernelModulemgr_stub
  SceIofilemgr_stub
  SceLibKernel_stub
  png
  jpeg
  z
  m
  c
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME})

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
