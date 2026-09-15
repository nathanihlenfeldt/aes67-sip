# Unit and integration tests.  They run on any platform: the AES67 and SIP
# planes are exercised through the null audio backend, the simulated daemon and
# the stub SIP engine.
add_executable(aes67-sip-tests
  test_main.cpp
  test_config.cpp
  test_audio.cpp
  test_daemon_client.cpp
  test_gateway.cpp)

target_link_libraries(aes67-sip-tests PRIVATE aes67-sip-core)
target_compile_options(aes67-sip-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Same rpath as the gateway: the tests link the shared pjproject libraries.
if(AES67_SIP_PJSIP_RPATH)
  set_target_properties(aes67-sip-tests PROPERTIES
    BUILD_RPATH "${AES67_SIP_PJSIP_RPATH}"
    INSTALL_RPATH "${AES67_SIP_PJSIP_RPATH}")
endif()

add_test(NAME aes67-sip-tests COMMAND aes67-sip-tests)

# The gateway can also be smoke tested end to end from the command line:
#   build/aes67-sip -c config/aes67-sip.dev.conf --validate
add_test(NAME config-validate
  COMMAND aes67-sip -c ${CMAKE_SOURCE_DIR}/config/aes67-sip.dev.conf --validate)
set_tests_properties(config-validate PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
