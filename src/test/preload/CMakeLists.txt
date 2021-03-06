#################################
## build the preload libraries ##
#################################

## this one has no constructor, preloads time() and does dlsym() lookup there
add_library(test-preload-interpose SHARED test_preload_interpose1.c)
target_link_libraries(test-preload-interpose ${DL_LIBRARIES})

## this one has a constructor in which it does a dlsym() lookup
add_library(test-preload-interpose-constructor SHARED test_preload_interpose1.c test_preload_interpose_constructor.c)
target_link_libraries(test-preload-interpose-constructor ${DL_LIBRARIES})

## this one meant for chaining preloads
add_library(test-preload-interpose2 SHARED test_preload_interpose2.c)
target_link_libraries(test-preload-interpose2 ${DL_LIBRARIES})

## define time and return a constant
add_library(test-preload-lib SHARED test_preload_lib.c)

## different test cases in library form
add_library(test-preload-lib-run1 SHARED test_preload_run1.c)
add_library(test-preload-lib-run2 SHARED test_preload_run2.c)

#######################################################################
## basic preload test from an exe and symbol lookup from constructor ##
#######################################################################

## glibc loader

add_executable(test-preload-exe1 test_preload_main.c test_preload_run1.c)
target_link_libraries(test-preload-exe1 test-preload-lib)

add_test(NAME preload-glibc-exe COMMAND test-preload-exe1)
SET_TESTS_PROPERTIES(preload-glibc-exe
    PROPERTIES ENVIRONMENT "LD_PRELOAD=${CMAKE_CURRENT_BINARY_DIR}/libtest-preload-interpose.so")

add_test(NAME preload-glibc-exe-constructor COMMAND test-preload-exe1)
SET_TESTS_PROPERTIES(preload-glibc-exe-constructor
    PROPERTIES ENVIRONMENT "LD_PRELOAD=${CMAKE_CURRENT_BINARY_DIR}/libtest-preload-interpose-constructor.so")

## elf loader

add_executable(test-preload-elf-exe1 test_preload_main.c test_preload_run1.c)
target_link_libraries(test-preload-elf-exe1 test-preload-lib)
set_target_properties(test-preload-elf-exe1 PROPERTIES 
    INSTALL_RPATH ${CMAKE_BINARY_DIR}/src/external/elf-loader
    INSTALL_RPATH_USE_LINK_PATH TRUE 
    LINK_FLAGS "-Wl,--no-as-needed,-rpath=${CMAKE_BINARY_DIR}/src/external/elf-loader,-dynamic-linker=${CMAKE_BINARY_DIR}/src/external/elf-loader/ldso"
)

add_test(NAME preload-elf-exe COMMAND test-preload-elf-exe1)
SET_TESTS_PROPERTIES(preload-elf-exe
    PROPERTIES ENVIRONMENT "LD_PRELOAD=${CMAKE_CURRENT_BINARY_DIR}/libtest-preload-interpose.so")

add_test(NAME preload-elf-exe-constructor COMMAND test-preload-elf-exe1)
SET_TESTS_PROPERTIES(preload-elf-exe-constructor
    PROPERTIES ENVIRONMENT "LD_PRELOAD=${CMAKE_CURRENT_BINARY_DIR}/libtest-preload-interpose-constructor.so")

###################################################################
## preload test for chaining symbol lookups across multiple libs ##
###################################################################

## glibc loader

add_executable(test-preload-exe2 test_preload_main.c test_preload_run2.c)
target_link_libraries(test-preload-exe2 test-preload-lib)

add_test(NAME preload-glibc-exe-chain COMMAND test-preload-exe2)
SET_TESTS_PROPERTIES(preload-glibc-exe-chain
    PROPERTIES ENVIRONMENT "LD_PRELOAD=${CMAKE_CURRENT_BINARY_DIR}/libtest-preload-interpose.so:${CMAKE_CURRENT_BINARY_DIR}/libtest-preload-interpose2.so")


## elf loader

add_executable(test-preload-elf-exe2 test_preload_main.c test_preload_run2.c)
target_link_libraries(test-preload-elf-exe2 test-preload-lib)
set_target_properties(test-preload-elf-exe2 PROPERTIES 
    INSTALL_RPATH ${CMAKE_BINARY_DIR}/src/external/elf-loader
    INSTALL_RPATH_USE_LINK_PATH TRUE 
    LINK_FLAGS "-Wl,--no-as-needed,-rpath=${CMAKE_BINARY_DIR}/src/external/elf-loader,-dynamic-linker=${CMAKE_BINARY_DIR}/src/external/elf-loader/ldso"
)

add_test(NAME preload-elf-exe-chain COMMAND test-preload-elf-exe2)
SET_TESTS_PROPERTIES(preload-elf-exe-chain
    PROPERTIES ENVIRONMENT "LD_PRELOAD=${CMAKE_CURRENT_BINARY_DIR}/libtest-preload-interpose.so:${CMAKE_CURRENT_BINARY_DIR}/libtest-preload-interpose2.so")

#####################################################
## test preloading functions called from a library ##
#####################################################

## glibc loader

add_executable(test-preload-exe-run1 test_preload_main.c)
target_link_libraries(test-preload-exe-run1 test-preload-lib-run1 test-preload-lib)

add_test(NAME preload-glibc-lib COMMAND test-preload-exe-run1)
SET_TESTS_PROPERTIES(preload-glibc-lib
    PROPERTIES ENVIRONMENT "LD_PRELOAD=${CMAKE_CURRENT_BINARY_DIR}/libtest-preload-interpose.so")

add_test(NAME preload-glibc-lib-constructor COMMAND test-preload-exe-run1)
SET_TESTS_PROPERTIES(preload-glibc-lib-constructor
    PROPERTIES ENVIRONMENT "LD_PRELOAD=${CMAKE_CURRENT_BINARY_DIR}/libtest-preload-interpose-constructor.so")

add_executable(test-preload-exe-run2 test_preload_main.c)
target_link_libraries(test-preload-exe-run2 test-preload-lib-run2 test-preload-lib)

add_test(NAME preload-glibc-lib-chain COMMAND test-preload-exe-run2)
SET_TESTS_PROPERTIES(preload-glibc-lib-chain
    PROPERTIES ENVIRONMENT "LD_PRELOAD=${CMAKE_CURRENT_BINARY_DIR}/libtest-preload-interpose.so:${CMAKE_CURRENT_BINARY_DIR}/libtest-preload-interpose2.so")


## elf loader

add_executable(test-preload-elf-exe-run1 test_preload_main.c)
target_link_libraries(test-preload-elf-exe-run1 test-preload-lib-run1 test-preload-lib)
set_target_properties(test-preload-elf-exe-run1 PROPERTIES 
    INSTALL_RPATH ${CMAKE_BINARY_DIR}/src/external/elf-loader
    INSTALL_RPATH_USE_LINK_PATH TRUE 
    LINK_FLAGS "-Wl,--no-as-needed,-rpath=${CMAKE_BINARY_DIR}/src/external/elf-loader,-dynamic-linker=${CMAKE_BINARY_DIR}/src/external/elf-loader/ldso"
)

add_test(NAME preload-elf-lib COMMAND test-preload-exe-run1)
SET_TESTS_PROPERTIES(preload-elf-lib
    PROPERTIES ENVIRONMENT "LD_PRELOAD=${CMAKE_CURRENT_BINARY_DIR}/libtest-preload-interpose.so")

add_test(NAME preload-elf-lib-constructor COMMAND test-preload-exe-run1)
SET_TESTS_PROPERTIES(preload-elf-lib-constructor
    PROPERTIES ENVIRONMENT "LD_PRELOAD=${CMAKE_CURRENT_BINARY_DIR}/libtest-preload-interpose-constructor.so")

add_executable(test-preload-elf-exe-run2 test_preload_main.c)
target_link_libraries(test-preload-elf-exe-run2 test-preload-lib-run2 test-preload-lib)
set_target_properties(test-preload-elf-exe-run2 PROPERTIES 
    INSTALL_RPATH ${CMAKE_BINARY_DIR}/src/external/elf-loader
    INSTALL_RPATH_USE_LINK_PATH TRUE 
    LINK_FLAGS "-Wl,--no-as-needed,-rpath=${CMAKE_BINARY_DIR}/src/external/elf-loader,-dynamic-linker=${CMAKE_BINARY_DIR}/src/external/elf-loader/ldso"
)

add_test(NAME preload-elf-lib-chain COMMAND test-preload-exe-run2)
SET_TESTS_PROPERTIES(preload-elf-lib-chain
    PROPERTIES ENVIRONMENT "LD_PRELOAD=${CMAKE_CURRENT_BINARY_DIR}/libtest-preload-interpose.so:${CMAKE_CURRENT_BINARY_DIR}/libtest-preload-interpose2.so")


#######################################################################
## test preloading functions called from a dynamically loaded plugin ##
#######################################################################

## glibc loader

# note - in order for this to work correctly in glibc, you must link to the interpose library
# this requirement is not ideal - the loaded plugin should not care about preloading
add_library(test-preload-dl-glibc-run1-lib SHARED test_preload_run1.c)
target_link_libraries(test-preload-dl-glibc-run1-lib test-preload-interpose test-preload-lib)

add_executable(test-preload-dl-glibc-run1-exe test_preload_dl_main.c)
target_link_libraries(test-preload-dl-glibc-run1-exe ${DL_LIBRARIES})

add_test(NAME preload-glibc-dl COMMAND test-preload-dl-glibc-run1-exe ${CMAKE_CURRENT_BINARY_DIR}/libtest-preload-dl-glibc-run1-lib.so)
SET_TESTS_PROPERTIES(preload-glibc-dl
    PROPERTIES ENVIRONMENT "LD_PRELOAD=${CMAKE_CURRENT_BINARY_DIR}/libtest-preload-interpose.so")

## elf loader

add_library(test-preload-dl-elf-run1-lib SHARED test_preload_run1.c)
target_link_libraries(test-preload-dl-elf-run1-lib test-preload-lib)

# note - in order for this to work in elf loader, you must link to the library that
# is also linked in the dynamically loaded plugin
# o/w, b/c of the search order, dlsym("time") from interpose.so will find the symbol
# in the libc dependency of the executable before the library dependency of the
# dynamically loaded plugin
add_executable(test-preload-dl-elf-run1-exe test_preload_dl_main.c)
target_link_libraries(test-preload-dl-elf-run1-exe test-preload-lib ${DL_LIBRARIES})
set_target_properties(test-preload-dl-elf-run1-exe PROPERTIES 
    INSTALL_RPATH ${CMAKE_BINARY_DIR}/src/external/elf-loader
    INSTALL_RPATH_USE_LINK_PATH TRUE 
    LINK_FLAGS "-Wl,--no-as-needed,-rpath=${CMAKE_BINARY_DIR}/src/external/elf-loader,-dynamic-linker=${CMAKE_BINARY_DIR}/src/external/elf-loader/ldso"
)

add_test(NAME preload-elf-dl COMMAND test-preload-dl-elf-run1-exe ${CMAKE_CURRENT_BINARY_DIR}/libtest-preload-dl-elf-run1-lib.so)
SET_TESTS_PROPERTIES(preload-elf-dl
    PROPERTIES ENVIRONMENT "LD_PRELOAD=${CMAKE_CURRENT_BINARY_DIR}/libtest-preload-interpose.so")

## shadow

## shadow itself already dynamically loads the plugin, so just combine main() and run_test()
add_library(shadow-plugin-test-preload SHARED test_preload_plugin_main.c test_preload_run1.c)
target_link_libraries(shadow-plugin-test-preload test-preload-lib)

## register the shadow tests
## test loading the preloads with the environment variables
add_test(
    NAME preload-shadow-dl-env
    COMMAND sh -c "\
    ${yaml2xml} ${CMAKE_CURRENT_SOURCE_DIR}/preload-env.test.shadow.config.yaml --output - \
    | ${CMAKE_BINARY_DIR}/src/main/shadow -l debug -d preload-env.shadow.data - \
    "
)
SET_TESTS_PROPERTIES(preload-shadow-dl-env
    PROPERTIES ENVIRONMENT "LD_PRELOAD=${CMAKE_CURRENT_BINARY_DIR}/libtest-preload-interpose.so:${CMAKE_BINARY_DIR}/src/preload/libshadow-interpose.so;LD_STATIC_TLS_EXTRA=10240000")
## test loading the preload libs at runtime with elf-loader
add_test(
    NAME preload-shadow-dl-run
    COMMAND sh -c "\
    ${yaml2xml} ${CMAKE_CURRENT_SOURCE_DIR}/preload-run.test.shadow.config.yaml --output - \
    | ${CMAKE_BINARY_DIR}/src/main/shadow -l debug -d preload-run.shadow.data - \
    "
)
