cmake_minimum_required(VERSION 3.21)
project(pkmn_gen3 LANGUAGES CXX)

# The engine is header-only: this target is just its include path and C++20.
# A driver (birch) uses it with add_subdirectory and target_link_libraries.
add_library(pkmn_gen3 INTERFACE)
add_library(pkmn::gen3 ALIAS pkmn_gen3)
target_include_directories(pkmn_gen3 INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(pkmn_gen3 INTERFACE cxx_std_20)

if(NOT PROJECT_IS_TOP_LEVEL)
  return()
endif()

# Everything below is tests and tools. Use the presets (CMakePresets.json):
# `debug` runs them under ASan and UBSan with asserts on, `release` with -O3
# and asserts off.
enable_testing()

set(CMAKE_CXX_EXTENSIONS OFF)
add_library(pkmn_warnings INTERFACE)
target_compile_options(pkmn_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror)

# Each header compiles on its own, and every header's static_asserts run.
# All of them are linked into one binary, which also checks that the headers
# define nothing twice across translation units.
file(GLOB_RECURSE PKMN_HEADERS CONFIGURE_DEPENDS
     RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp)
set(PKMN_HEADER_TUS)
foreach(header ${PKMN_HEADERS})
  string(MAKE_C_IDENTIFIER ${header} name)
  set(tu ${CMAKE_CURRENT_BINARY_DIR}/header_tus/${name}.cpp)
  file(CONFIGURE OUTPUT ${tu} CONTENT "#include \"${header}\"\n")
  list(APPEND PKMN_HEADER_TUS ${tu})
endforeach()
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/header_tus/main.cpp
     CONTENT "int main() { return 0; }\n")
add_executable(headers ${PKMN_HEADER_TUS} ${CMAKE_CURRENT_BINARY_DIR}/header_tus/main.cpp)
target_link_libraries(headers PRIVATE pkmn_gen3 pkmn_warnings)
add_test(NAME headers COMMAND headers)

add_executable(check_data tools/check_data.cpp)
target_link_libraries(check_data PRIVATE pkmn_gen3 pkmn_warnings)
add_test(NAME check_data COMMAND check_data)

# damage.hpp against Showdown, from the committed damage_cases.inc
# (regenerate with tools/damage_oracle.cjs).
add_executable(check_damage tools/check_damage.cpp)
target_link_libraries(check_damage PRIVATE pkmn_gen3 pkmn_warnings)
add_test(NAME check_damage COMMAND check_damage)

# The team corpus lives in birch, beside this repo.
set(PKMN_TEAMS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../teams/adv
    CACHE PATH "Showdown export teams for check_teams")
add_executable(check_teams tools/check_teams.cpp)
target_link_libraries(check_teams PRIVATE pkmn_gen3 pkmn_warnings)
file(GLOB PKMN_TEAM_FILES ${PKMN_TEAMS_DIR}/*.txt)
list(FILTER PKMN_TEAM_FILES EXCLUDE REGEX "/all\\.txt$")
if(PKMN_TEAM_FILES)
  # Known bad: HP [Fire] with IVs for HP Dark. Showdown's validator rejects it
  # too (team-validator.ts, "its IVs are for Hidden Power").
  add_test(NAME check_teams COMMAND check_teams
           --expect "[gen3ou] Johnald Teamdump/Leer Mence" ${PKMN_TEAM_FILES})
else()
  message(WARNING "No teams in ${PKMN_TEAMS_DIR}; check_teams won't run")
endif()
