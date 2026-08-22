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
#define MAPPLE_PROTOCOL_VERSION 2U

#define MAPPLE_HEADER_FLAG_STREAM (1U << 0)
#define MAPPLE_HEADER_KNOWN_FLAGS MAPPLE_HEADER_FLAG_STREAM
#define MAPPLE_PSLIST_COMM_SIZE 16U

enum mapple_command_id {
	MAPPLE_COMMAND_ECHO = 1,
	MAPPLE_COMMAND_PSLIST = 2,
	MAPPLE_COMMAND_COUNT
};

struct mapple_wire_header {
	__u32 magic;
	__u16 version;
	__u16 command;
	__u32 flags;
	__u64 payload_size;
} PACKED_STRUCT;

enum mapple_pslist_record_type {
	MAPPLE_PSLIST_RECORD_TASK = 1,
	MAPPLE_PSLIST_RECORD_PROCESS = 2
};

struct mapple_pslist_record_header {
	__u32 record_size;
	__u16 record_type;
	__u16 reserved;
} PACKED_STRUCT;

struct mapple_pslist_process_record {
	struct mapple_pslist_record_header header;
	__u32 tgid;
	__u32 ppid;
	__u32 command_line_length;
} PACKED_STRUCT;

struct mapple_pslist_task_record {
	struct mapple_pslist_record_header header;
	__u32 tid;
	__u32 tgid;
	__u8 state;
	__u8 reserved[3];
	char comm[MAPPLE_PSLIST_COMM_SIZE];
} PACKED_STRUCT;

#undef PACKED_STRUCT

#endif /* MAPPLE_COMMANDS_H */
