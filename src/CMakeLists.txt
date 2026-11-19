set(xlisp_sources
    xlansi.c
    xlapi.c
    xlbcode.h
    xlcobj.c
    xlcom.c
    xldbg.c
    xldmem.c
    xlfasl.c
    xlftab.c
    xlfun1.c
    xlfun2.c
    xlfun3.c
    xlimage.c
    xlinit.c
    xlint.c
    xlio.c
    xlitersq.c
    xlmain.c
    xlmath.c
    xlobj.c
    xlosint.c
    xlprint.c
    xlread.c
    xlsym.c
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/xlisp.h")

if(WIN32)
    list(APPEND xlisp_sources msstuff.c)
else()
    list(APPEND xlisp_sources unstuff.c)
endif()

target_sources(xlisp PRIVATE ${xlisp_sources})
target_include_directories(xlisp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)

# The IDE host links this library into executables and shared libraries.
set_target_properties(xlisp PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT WIN32)
    target_link_libraries(xlisp PUBLIC m)
endif()
if(MSVC)
    target_compile_definitions(xlisp PUBLIC _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
endif()
