# http: HTTP/1.1 as pure logic. HttpLexer cuts the byte stream into
# tokens. For a server, HttpConnection's state machine assembles requests
# from them, routes them (HttpRoutes) and checks they were answered
# (HttpResponse); hw/freertos/xHttpServer runs it, a thread per client.
# For a client, HttpClientProtocol has URLs, the request head and
# HttpResponseReader; hw/freertos/xHttpClient runs it on the caller's thread.
#
# Added by iNetTransport/CMakeLists.txt. Needs nothing else.
add_library(inet_http STATIC
    inc/HttpToken.h
    inc/HttpLexer.h
    inc/HttpRequest.h
    inc/HttpResponse.h
    inc/HttpConnection.h
    inc/HttpClientProtocol.h
    src/HttpLexer.cpp
    src/HttpRequest.cpp
    src/HttpResponse.cpp
    src/HttpConnection.cpp
    src/HttpClientProtocol.cpp
)

target_include_directories(inet_http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_compile_features(inet_http PUBLIC cxx_std_17)
set_target_properties(inet_http PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${INET_LIB_DIR})

if(SENSOR_FW_BUILD_TESTS)
    add_executable(Http_test test/Http_test.cpp)
    target_link_libraries(Http_test PRIVATE inet_http)
    add_test(NAME Http_test COMMAND Http_test)
    add_executable(HttpClient_test test/HttpClient_test.cpp)
    target_link_libraries(HttpClient_test PRIVATE inet_http)
    add_test(NAME HttpClient_test COMMAND HttpClient_test)
endif()
