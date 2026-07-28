# Toolchain for the Amiga (m68k) target. vbcc comes from the terriblefire78/vbcc docker
# image (see vbcc.Dockerfile), pulled and built on first configure:
#   cmake -B build-amiga -DCMAKE_TOOLCHAIN_FILE=cmake/amiga-vbcc.cmake
#   cmake --build build-amiga

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR m68k)
set(AMIGA TRUE)

set(VBCC_IMAGE rlaunch-vbcc)

find_program(DOCKER docker REQUIRED)

execute_process(COMMAND ${DOCKER} image inspect ${VBCC_IMAGE} RESULT_VARIABLE res OUTPUT_QUIET ERROR_QUIET)
if(NOT res EQUAL 0)
	message(STATUS "Building the ${VBCC_IMAGE} docker image")
	execute_process(
		COMMAND ${DOCKER} build -t ${VBCC_IMAGE} -f ${CMAKE_CURRENT_LIST_DIR}/vbcc.Dockerfile ${CMAKE_CURRENT_LIST_DIR}
		RESULT_VARIABLE res)
	if(NOT res EQUAL 0)
		message(FATAL_ERROR "docker build of ${VBCC_IMAGE} failed")
	endif()
endif()

# vc runs inside the container; the source and build trees are mounted at their host paths
# so every path cmake puts on the command line stays valid inside.
set(VBCC_WRAPPER ${CMAKE_BINARY_DIR}/vc-docker)
file(WRITE ${VBCC_WRAPPER} "#!/bin/sh
exec ${DOCKER} run --rm -u $(id -u):$(id -g) \\
	-v \"${CMAKE_SOURCE_DIR}:${CMAKE_SOURCE_DIR}\" -v \"${CMAKE_BINARY_DIR}:${CMAKE_BINARY_DIR}\" \\
	-w \"$PWD\" ${VBCC_IMAGE} vc +aos68k \"$@\"
")
file(CHMOD ${VBCC_WRAPPER} PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

set(CMAKE_C_COMPILER ${VBCC_WRAPPER})
set(CMAKE_C_COMPILER_FORCED TRUE)

# the image's vc config refers to $VBCC/... which vc doesn't expand for the compiler, so spell it out.
# netinclude comes first: its proto/socket.h is the one that has matching prototypes.
set(NDK /opt/vbcc/NDK3.2)
set(CMAKE_C_FLAGS_INIT "-I${NDK}/SANA+RoadshowTCP-IP/netinclude -I${NDK}/Include_H \
-I/opt/vbcc/targets/m68k-amigaos/include \
-warn=-1 -dontwarn=163 -dontwarn=307 -dontwarn=65 -dontwarn=166 -dontwarn=167 -dontwarn=81")
set(CMAKE_C_FLAGS_RELEASE_INIT "-O2 -DNDEBUG")
