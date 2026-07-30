cmake_minimum_required(VERSION 3.19)
project(rlaunch C)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(GEN ${CMAKE_BINARY_DIR}/generated)
add_custom_command(
	OUTPUT ${GEN}/rlnet.c ${GEN}/rlnet.h
	COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN}
	COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/src/mkmsg.py ${GEN}/rlnet ${CMAKE_SOURCE_DIR}/src/rlnet.msg
	DEPENDS ${CMAKE_SOURCE_DIR}/src/mkmsg.py ${CMAKE_SOURCE_DIR}/src/rlnet.msg
	COMMENT "Generating rlnet message codecs")

# OBJECT library, not STATIC - vbcc ships no archiver
add_library(common OBJECT
	src/util.c src/transport.c src/peer.c src/protocol.c src/socket_includes.c
	${GEN}/rlnet.c)
target_include_directories(common PUBLIC src ${GEN})

if(AMIGA)
	target_compile_definitions(common PUBLIC __AMIGA__ NO_C_LIB)
elseif(MSVC)
	target_compile_options(common PUBLIC /W4 /wd4127 /wd4100)
	target_link_libraries(common PUBLIC ws2_32.lib)
else()
	target_compile_options(common PUBLIC -Wall)
	if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
		target_compile_definitions(common PUBLIC _GNU_SOURCE)
	endif()
endif()

add_executable(rl-target src/target.c $<$<BOOL:${AMIGA}>:src/amigafs.c>)
target_link_libraries(rl-target PRIVATE common)

if(AMIGA)
	# Payload for the end-to-end test; rl-controller launches it on the Amiga.
	add_executable(rl-payload test/amiga_payload.c)
endif()

if(NOT AMIGA)
	add_executable(rl-controller src/controller.c src/file_server.c)
	target_link_libraries(rl-controller PRIVATE common)

	# rlaunch_test.c #includes file_server.c to reach its statics, so it must
	# not be linked in a second time.
	enable_testing()
	add_executable(rl-test test/rlaunch_test.c)
	target_link_libraries(rl-test PRIVATE common)
	add_test(NAME rlaunch COMMAND rl-test)

	# Black-box check on the real controller binary; needs a POSIX shell.
	if(UNIX)
		add_test(NAME controller-connect
			COMMAND ${CMAKE_SOURCE_DIR}/test/controller-connect-test.sh
				$<TARGET_FILE:rl-controller> ${Python3_EXECUTABLE})
	endif()

	add_test(NAME controller-spawn-failure
		COMMAND ${Python3_EXECUTABLE}
			${CMAKE_SOURCE_DIR}/test/controller-spawn-failure-test.py
			$<TARGET_FILE:rl-controller>)
endif()
