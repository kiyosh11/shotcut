find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Test)

add_executable(test_gpuinfo
  test_gpuinfo.cpp
  ${CMAKE_SOURCE_DIR}/src/gpuinfo.cpp
)
target_include_directories(test_gpuinfo PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_gpuinfo PRIVATE Qt6::Core Qt6::Test)
if(WIN32)
  target_link_libraries(test_gpuinfo PRIVATE dxgi dxguid)
endif()
add_test(NAME gpuinfo COMMAND test_gpuinfo)

add_executable(test_mcpundoutils
  test_mcpundoutils.cpp
)
target_include_directories(test_mcpundoutils PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_mcpundoutils PRIVATE Qt6::Gui Qt6::Test)
add_test(NAME mcpundoutils COMMAND test_mcpundoutils)
