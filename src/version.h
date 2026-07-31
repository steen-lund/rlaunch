#ifndef RLAUNCH_VERSION_H
#define RLAUNCH_VERSION_H

#define RLAUNCH_BASE_DEVICE_NAME "TBL"
#define RLAUNCH_VIRTUAL_INPUT_FILE "+virtual-input+"
#define RLAUNCH_VIRTUAL_OUTPUT_FILE "+virtual-output+"

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#define RLAUNCH_VER_MAJOR 1
#define RLAUNCH_VER_MINOR 1

#define RLAUNCH_VER_MAJOR_STR TOSTRING(RLAUNCH_VER_MAJOR)
#define RLAUNCH_VER_MINOR_STR TOSTRING(RLAUNCH_VER_MINOR)

#define RLAUNCH_VERSION RLAUNCH_VER_MAJOR_STR "." RLAUNCH_VER_MINOR_STR

/* What the handshake compares, which is not the release version: peers must
 * agree exactly, so a release that leaves the wire format alone must leave
 * these alone too or every already-installed target stops connecting. Bump
 * only when the message format changes. Still 1.0 as of release 1.1 -- the
 * 1.1 protocol work hardened the decoders without moving the format. */
#define RLAUNCH_PROTO_MAJOR 1
#define RLAUNCH_PROTO_MINOR 0

#define RLAUNCH_PROTO_VERSION TOSTRING(RLAUNCH_PROTO_MAJOR) "." TOSTRING(RLAUNCH_PROTO_MINOR)

#define RLAUNCH_LICENSE "Copyright (c)2009 Andreas Fredriksson, TBL Technologies. All rights reserved."

#endif
