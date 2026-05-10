# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico_w CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(Pico-W-NS-Example C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(Pico-W-NS-Example
    main.c 
)

pico_set_program_name(Pico-W-NS-Example "Pico-W-NS-Example")
pico_set_program_version(Pico-W-NS-Example "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(Pico-W-NS-Example 1)
pico_enable_stdio_usb(Pico-W-NS-Example 0)

# Add the standard library to the build
target_link_libraries(Pico-W-NS-Example
        pico_stdlib)

# Disable LWIP
target_compile_definitions(Pico-W-NS-Example
  INTERFACE
  CYW43_LWIP=0
)

# Add the standard include files to the build
target_include_directories(Pico-W-NS-Example PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

# Import NS-LIB-HID library
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/external/NS-LIB-HID")

# Add the NS-LIB-HID library
target_link_libraries(Pico-W-NS-Example
    ns_lib_hid
)

# Add USB related libraries
target_link_libraries(Pico-W-NS-Example 
    tinyusb_device
    tinyusb_board
)

# Add wireless networking libraries
target_link_libraries(Pico-W-NS-Example 
    pico_cyw43_arch_lwip_threadsafe_background 
    pico_cyw43_driver 
    pico_btstack_cyw43 
    pico_btstack_hci_transport_cyw43 
    pico_btstack_flash_bank
    pico_btstack_ble 
    pico_btstack_classic 
)

pico_add_extra_outputs(Pico-W-NS-Example)

