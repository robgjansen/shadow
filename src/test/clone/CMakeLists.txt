include_directories(${GLIB_INCLUDES})
add_executable(test_clone test_clone.c ../test_common.c)
target_compile_options(test_clone PUBLIC "-pthread")
target_link_libraries(test_clone ${GLIB_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})
add_linux_tests(BASENAME clone COMMAND test_clone)
add_shadow_tests(BASENAME clone METHODS hybrid ptrace)

# The clone test exercises some corner cases in memory management, particularly
# when the thread leader exits before all the threads. Useful to test it without
# the memory manager (really the MemoryMapper) enabled.
add_shadow_tests(BASENAME clone-nomm METHODS hybrid ptrace ARGS --use-memory-manager=false)

# FIXME: Enable in all configurations. See https://github.com/shadow/shadow/issues/892
# FIXME: The interposed libc does not handle this either.
#add_shadow_tests(BASENAME clone METHODS preload CONFIGURATIONS ilibc)