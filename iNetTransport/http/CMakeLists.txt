# http: an HTTP/1.1 server's pure logic. HttpLexer cuts the byte stream
# into tokens; HttpConnection's state machine assembles requests from
# them, routes them (HttpRoutes) and checks they were answered
# (HttpResponse). hw/freertos/xHttpServer runs it, a thread per client.
#
# Added by iNetTransport/CMakeLists.txt. Needs nothing else.
add_library(inet_http STATIC
    inc/HttpToken.h
    inc/HttpLexer.h
    inc/HttpRequest.h
    inc/HttpResponse.h
    inc/HttpConnection.h
    src/HttpLexer.cpp
    src/HttpRequest.cpp
    src/HttpResponse.cpp
    src/HttpConnection.cpp
)

target_include_directories(inet_http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_compile_features(inet_http PUBLIC cxx_std_17)
set_target_properties(inet_http PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${INET_LIB_DIR})

if(SENSOR_FW_BUILD_TESTS)
    add_executable(Http_test test/Http_test.cpp)
    target_link_libraries(Http_test PRIVATE inet_http)
    add_test(NAME Http_test COMMAND Http_test)
endif()
