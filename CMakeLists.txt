cmake_minimum_required(VERSION 3.21)
project(iocp_echo_server LANGUAGES CXX)

# =============================================================
#  기본 설정
# =============================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
  message(FATAL_ERROR "이 프로젝트는 IOCP 를 사용하므로 Windows 전용이다.")
endif()

# 산출물을 한 곳에 모은다. libmysql.dll 복사 대상도 여기가 된다
set(OUT_DIR ${CMAKE_BINARY_DIR}/bin)
foreach(cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY${cfg} ${OUT_DIR})
endforeach()

# =============================================================
#  옵션
# =============================================================
option(ECHO_ENABLE_ASAN "AddressSanitizer 활성화" OFF)
option(ECHO_BUILD_TESTS "단위 테스트 빌드" ON)
option(ECHO_BUILD_BENCH "부하 테스트 봇 빌드" ON)

# MySQL 설치 경로. 다른 위치면 -DMYSQL_ROOT=... 로 덮어쓴다
set(MYSQL_ROOT "C:/Program Files/MySQL/MySQL Server 8.4"
    CACHE PATH "MySQL Server 설치 경로")

find_path(MYSQL_INCLUDE_DIR mysql.h
          HINTS "${MYSQL_ROOT}/include" NO_DEFAULT_PATH)
find_library(MYSQL_LIBRARY NAMES libmysql
             HINTS "${MYSQL_ROOT}/lib" NO_DEFAULT_PATH)
find_file(MYSQL_RUNTIME_DLL libmysql.dll
          HINTS "${MYSQL_ROOT}/lib" NO_DEFAULT_PATH)

if(NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_LIBRARY)
  message(FATAL_ERROR
    "MySQL 개발 파일을 찾지 못했다.\n"
    "  탐색 경로 : ${MYSQL_ROOT}\n"
    "  해결      : -DMYSQL_ROOT=\"<MySQL Server 경로>\" 로 지정한다")
endif()
message(STATUS "MySQL include : ${MYSQL_INCLUDE_DIR}")
message(STATUS "MySQL library : ${MYSQL_LIBRARY}")

# =============================================================
#  공통 컴파일 설정
# =============================================================
add_library(echo_common_flags INTERFACE)
target_compile_options(echo_common_flags INTERFACE
  /W4                 # 경고 수준
  /permissive-        # 표준 준수 모드
  /utf-8              # 소스 · 실행 문자셋 UTF-8. 한글 주석 깨짐 방지
  /Zc:preprocessor
  /EHsc
)
target_compile_definitions(echo_common_flags INTERFACE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _WIN32_WINNT=0x0A00   # Windows 10 이상. AcceptEx · ConnectEx 사용
)

if(ECHO_ENABLE_ASAN)
  message(STATUS "AddressSanitizer 활성화")
  target_compile_options(echo_common_flags INTERFACE /fsanitize=address /Zi)
  target_link_options(echo_common_flags INTERFACE /INCREMENTAL:NO)
endif()

# =============================================================
#  echo_core : 서버와 봇이 공유하는 계층
# =============================================================
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/src/net/*.cpp
     ${CMAKE_SOURCE_DIR}/src/proto/*.cpp
     ${CMAKE_SOURCE_DIR}/src/core/*.cpp
     ${CMAKE_SOURCE_DIR}/src/db/*.cpp
     ${CMAKE_SOURCE_DIR}/src/app/*.cpp)

add_library(echo_core STATIC ${CORE_SOURCES})
target_include_directories(echo_core PUBLIC
  ${CMAKE_SOURCE_DIR}/src
  ${MYSQL_INCLUDE_DIR})
target_link_libraries(echo_core
  PUBLIC  echo_common_flags
  PRIVATE ws2_32 mswsock ${MYSQL_LIBRARY})

# =============================================================
#  실행 파일
# =============================================================
add_executable(echo_server ${CMAKE_SOURCE_DIR}/src/main.cpp)
target_link_libraries(echo_server PRIVATE echo_core)

if(ECHO_BUILD_BENCH)
  add_executable(echo_bot ${CMAKE_SOURCE_DIR}/bench/bot_client.cpp)
  target_link_libraries(echo_bot PRIVATE echo_core)
endif()

if(ECHO_BUILD_TESTS)
  file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/*.cpp)
  if(TEST_SOURCES)
    add_executable(echo_tests ${TEST_SOURCES})
    target_include_directories(echo_tests PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
    target_link_libraries(echo_tests PRIVATE echo_core)

    enable_testing()
    add_test(NAME unit COMMAND echo_tests)
  endif()
endif()

# =============================================================
#  런타임 준비
#    libmysql.dll 이 실행 파일 옆에 없으면 기동 시점에 실패한다
# =============================================================
if(MYSQL_RUNTIME_DLL)
  add_custom_command(TARGET echo_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MYSQL_RUNTIME_DLL}" "$<TARGET_FILE_DIR:echo_server>"
    COMMENT "libmysql.dll 복사")
endif()

# 설정 파일도 실행 파일 옆으로. db.ini 가 없으면 견본을 대신 놓는다
add_custom_command(TARGET echo_server POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:echo_server>/config"
  COMMENT "config 디렉터리 준비")
