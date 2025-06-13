cmake_minimum_required(VERSION 3.26)
project(PacketLoss CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED libdpdk)

add_executable(dpdk_packet_loss
    src/dpdk_packet_loss.cc)
    
target_compile_definitions(dpdk_packet_loss PRIVATE RTE_FORCE_INTRINSICS)
target_compile_options(dpdk_packet_loss PRIVATE -Wall -Wextra -Wpedantic -Werror -Wconversion)

target_include_directories(dpdk_packet_loss
    PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}"
	"${DPDK_INCLUDE_DIRS}"
)


target_link_libraries(dpdk_packet_loss
    PRIVATE
	"${DPDK_LIBRARIES}"
)
