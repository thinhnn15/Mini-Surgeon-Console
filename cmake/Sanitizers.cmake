if(MSC_SANITIZE AND MSC_TSAN)
    message(FATAL_ERROR "ASan and TSan cannot be used in the same build. Pick MSC_SANITIZE or MSC_TSAN.")
endif()

if((MSC_SANITIZE OR MSC_TSAN) AND MSVC)
    message(FATAL_ERROR "The sanitizer options are for GCC/Clang. On MSVC use /fsanitize=address by hand.")
endif()

if(MSC_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

# Finds data races: two threads touching the same memory, at least one writing, with no
# happens-before between them. Needs the code under test to actually run on several threads.
if(MSC_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()
