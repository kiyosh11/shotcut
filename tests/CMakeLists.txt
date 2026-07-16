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

add_executable(test_mcpbridgepolicy
  test_mcpbridgepolicy.cpp
)
target_include_directories(test_mcpbridgepolicy PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_mcpbridgepolicy PRIVATE Qt6::Core Qt6::Test)
add_test(NAME mcpbridgepolicy COMMAND test_mcpbridgepolicy)

add_executable(test_mltxmlproperty
  test_mltxmlproperty.cpp
)
target_include_directories(test_mltxmlproperty PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_mltxmlproperty PRIVATE Qt6::Core Qt6::Test)
add_test(NAME mltxmlproperty COMMAND test_mltxmlproperty)

add_executable(test_mcpxmlpathvalidator
  test_mcpxmlpathvalidator.cpp
  ${CMAKE_SOURCE_DIR}/src/mcpxmlpathvalidator.cpp
)
target_include_directories(test_mcpxmlpathvalidator PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_mcpxmlpathvalidator PRIVATE Qt6::Core Qt6::Test)
add_test(NAME mcpxmlpathvalidator COMMAND test_mcpxmlpathvalidator)

add_executable(test_mcpexporttargetpolicy
  test_mcpexporttargetpolicy.cpp
  ${CMAKE_SOURCE_DIR}/src/mcpexporttargetpolicy.cpp
)
target_include_directories(test_mcpexporttargetpolicy PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_mcpexporttargetpolicy PRIVATE Qt6::Core Qt6::Test)
add_test(NAME mcpexporttargetpolicy COMMAND test_mcpexporttargetpolicy)
