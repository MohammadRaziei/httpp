# -DUTILS_LIB=<libhttpp_utils> -DCONSUMER=<utils_consumer>
# Fails when libhttpp_utils needs the HTTP stack, or the consumer of httpp::utils pulls libhttpp in.
if(APPLE)
    set(_tool otool -L)
else()
    set(_tool readelf -d)
endif()

function(_needed file out)
    execute_process(COMMAND ${_tool} ${file} OUTPUT_VARIABLE text RESULT_VARIABLE rc ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${_tool} ${file} failed: ${err}")
    endif()
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

_needed(${UTILS_LIB} lib_text)
_needed(${CONSUMER} consumer_text)

# libhttpp.so / libhttpp.dylib (not libhttpp_utils), liburlparser, mbedtls
set(_forbidden "libhttpp\\.|liburlparser|libmbed|libtf-psa|libssl|libcrypto|libcurl")
if(lib_text MATCHES "${_forbidden}")
    message(FATAL_ERROR "libhttpp_utils depends on the HTTP stack:\n${lib_text}")
endif()
if(consumer_text MATCHES "libhttpp\\.|liburlparser|libmbed")
    message(FATAL_ERROR "a program that links only httpp::utils has libhttpp or urlparser among its dependencies:\n${consumer_text}")
endif()
message(STATUS "httpp_utils: no dependency on libhttpp, urlparser or mbedtls")
