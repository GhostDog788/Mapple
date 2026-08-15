#ifndef MAPPLE_COMMANDS_H
#define MAPPLE_COMMANDS_H

/*
 * Public wire ABI.  Multi-byte values are encoded in network byte order.
 * Keep this header usable by both the kernel module and ordinary C clients.
 */
#ifdef __KERNEL__
#include <linux/types.h>
typedef __be16 mapple_wire_u16;
typedef __be32 mapple_wire_u32;
typedef __be64 mapple_wire_u64;
#define MAPPLE_WIRE_PACKED __packed
#else
#include <stdint.h>
typedef uint16_t mapple_wire_u16;
typedef uint32_t mapple_wire_u32;
typedef uint64_t mapple_wire_u64;
#define MAPPLE_WIRE_PACKED __attribute__((packed))
#endif

#define MAPPLE_PROTOCOL_MAGIC 0x4d41504cU
#define MAPPLE_PROTOCOL_VERSION 1U
#define MAPPLE_WIRE_HEADER_SIZE 16U

enum mapple_command_id {
	MAPPLE_COMMAND_ECHO = 1,
};

struct mapple_wire_header {
	mapple_wire_u32 magic;
	mapple_wire_u16 version;
	mapple_wire_u16 command;
	mapple_wire_u64 payload_size;
} MAPPLE_WIRE_PACKED;

#undef MAPPLE_WIRE_PACKED

#endif /* MAPPLE_COMMANDS_H */
