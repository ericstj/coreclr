project(bundle)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(bundle_SOURCES bundle.cpp)

add_definitions(-DFX_VER_INTERNALNAME_STR=bundle.exe)

add_executable_clr(bundle
  ${bundle_SOURCES}
)

if(CLR_CMAKE_PLATFORM_UNIX)
    target_link_libraries(bundle
        coreclrpal
        palrt
    )
else()
    target_link_libraries(bundle
        utilcodestaticnohost
        advapi32.lib
        oleaut32.lib
        user32.lib
        ${STATIC_MT_CRT_LIB}
        ${STATIC_MT_VCRT_LIB}
    )
endif(CLR_CMAKE_PLATFORM_UNIX)

# add the install targets
install_clr(bundle)
