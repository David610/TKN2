cmake_minimum_required(VERSION 3.10)
project(TKN2 VERSION 1.0 LANGUAGES C)
set(CMAKE_C_STANDARD 99)
add_executable(build/webserver src/webserver.c)

# Enable packaging
set(CPACK_GENERATOR "TGZ")
set(CPACK_PACKAGE_NAME "praxis1")
set(CPACK_PACKAGE_VERSION "1.0.0")
set(CMAKE_C_STANDARD_REQUIRED True)
include(CPack)