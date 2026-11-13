# msc_set_warnings(<target> [NO_WERROR])
# Turns on a strict warning set. NO_WERROR is for targets that contain generated
# code we do not control (moc, qmlcachegen), so a Qt header change cannot break CI.
function(msc_set_warnings target)
    cmake_parse_arguments(ARG "NO_WERROR" "" "" ${ARGN})

    set(werror OFF)
    if(MSC_WERROR AND NOT ARG_NO_WERROR)
        set(werror ON)
    endif()

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- $<$<BOOL:${werror}>:/WX>)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
            $<$<BOOL:${werror}>:-Werror>)
    endif()
endfunction()
