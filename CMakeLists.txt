cmake_minimum_required(VERSION 3.10)
project(TKN2 VERSION 1.0 LANGUAGES C)
set(CMAKE_C_STANDARD 99)

add_executable(webserver src/webserver.c)

install(TARGETS webserver 
        RUNTIME DESTINATION bin)

install(FILES 
        ${CMAKE_SOURCE_DIR}/src/webserver.c
        ${CMAKE_SOURCE_DIR}/CMakeLists.txt
        ${CMAKE_SOURCE_DIR}/group.txt
        DESTINATION .)

set(CPACK_GENERATOR "TGZ")
set(CPACK_PACKAGE_NAME "praxis1")
set(CPACK_PACKAGE_FILE_NAME "praxis1")

set(CPACK_SOURCE_GENERATOR "TGZ")
set(CPACK_SOURCE_PACKAGE_FILE_NAME "praxis1")
set(CPACK_SOURCE_IGNORE_FILES
    "/build/"
    "/.git/"
    "/.venv/"
)

include(CPack)