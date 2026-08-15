#ifndef MAPPLE_COMMANDS_H
#define MAPPLE_COMMANDS_H

/*
 * Public wire ABI.  Multi-byte values are encoded in network byte order.
 * Keep this header usable by both the kernel module and ordinary C clients.
 */
#include <linux/types.h>

// prevents the compiler from inserting padding into the struct
#ifdef __KERNEL__
#define PACKED_STRUCT __packed
#else
#define PACKED_STRUCT __attribute__((packed))
#endif

#define MAPPLE_PROTOCOL_MAGIC 0x4d41504cU // "MAPL" in network byte order
#define MAPPLE_PROTOCOL_VERSION 1U

enum mapple_command_id {
	MAPPLE_COMMAND_ECHO = 1,
	MAPPLE_COMMAND_COUNT
};

struct mapple_wire_header {
	__u32 magic;
	__u16 version;
	__u16 command;
	__u64 payload_size;
} PACKED_STRUCT;

#undef PACKED_STRUCT

#endif /* MAPPLE_COMMANDS_H */
