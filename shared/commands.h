#ifndef MAPPLE_COMMANDS_H
#define MAPPLE_COMMANDS_H

/*
 * Public wire ABI. Multi-byte values are encoded in network byte order.
 * Keep this header usable by both the kernel module and ordinary C clients.
 */
#include <linux/types.h>

#ifdef __KERNEL__
#define PACKED_STRUCT __packed
#else
#define PACKED_STRUCT __attribute__((packed))
#endif

#define MAPPLE_PROTOCOL_MAGIC 0x4d41504cU /* "MAPL" in network byte order */
#define MAPPLE_PROTOCOL_VERSION 2U

#define MAPPLE_HEADER_FLAG_STREAM (1U << 0)
#define MAPPLE_HEADER_KNOWN_FLAGS MAPPLE_HEADER_FLAG_STREAM

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

#define MAPPLE_PSLIST_COMM_SIZE 16U
#define MAPPLE_PSLIST_COMMAND_LINE_MAX_SIZE (6U * 1024U * 1024U)
#define MAPPLE_PSLIST_FDINFO_MAX_SIZE (6U * 1024U * 1024U)

#define MAPPLE_PSLIST_GROUP_IDENTITY (1ULL << 0)
#define MAPPLE_PSLIST_GROUP_ENVIRONMENT (1ULL << 1)
#define MAPPLE_PSLIST_GROUP_SECURITY (1ULL << 2)
#define MAPPLE_PSLIST_GROUP_CONTEXT (1ULL << 3)
#define MAPPLE_PSLIST_GROUP_RESOURCES (1ULL << 4)
#define MAPPLE_PSLIST_GROUP_FDS (1ULL << 5)
#define MAPPLE_PSLIST_GROUP_FD_DETAILS (1ULL << 6)
#define MAPPLE_PSLIST_GROUP_NETWORK (1ULL << 7)
#define MAPPLE_PSLIST_GROUP_MEMORY_MAPS (1ULL << 8)
#define MAPPLE_PSLIST_GROUP_ELF (1ULL << 9)

#define MAPPLE_PSLIST_KNOWN_GROUPS                                                   \
	(MAPPLE_PSLIST_GROUP_IDENTITY | MAPPLE_PSLIST_GROUP_ENVIRONMENT |             \
	 MAPPLE_PSLIST_GROUP_SECURITY | MAPPLE_PSLIST_GROUP_CONTEXT |                 \
	 MAPPLE_PSLIST_GROUP_RESOURCES | MAPPLE_PSLIST_GROUP_FDS |                    \
	 MAPPLE_PSLIST_GROUP_FD_DETAILS | MAPPLE_PSLIST_GROUP_NETWORK |               \
	 MAPPLE_PSLIST_GROUP_MEMORY_MAPS | MAPPLE_PSLIST_GROUP_ELF)

#define MAPPLE_PSLIST_SUPPORTED_GROUPS                                               \
	(MAPPLE_PSLIST_GROUP_IDENTITY | MAPPLE_PSLIST_GROUP_FDS |                      \
	 MAPPLE_PSLIST_GROUP_FD_DETAILS | MAPPLE_PSLIST_GROUP_NETWORK)

#define MAPPLE_PSLIST_DEFAULT_GROUPS MAPPLE_PSLIST_SUPPORTED_GROUPS

struct mapple_pslist_request {
	__u64 groups;
} PACKED_STRUCT;

struct mapple_pslist_error_response {
	__u32 error_code;
	__u32 reserved;
	__u64 supported_groups;
} PACKED_STRUCT;

enum mapple_pslist_stream_record_type {
	MAPPLE_PSLIST_STREAM_SNAPSHOT_BEGIN = 1,
	MAPPLE_PSLIST_STREAM_TASK = 2,
	MAPPLE_PSLIST_STREAM_TASK_OBJECT_LINK = 3,
	MAPPLE_PSLIST_STREAM_OBJECT_BEGIN = 4,
	MAPPLE_PSLIST_STREAM_IDENTITY = 5,
	MAPPLE_PSLIST_STREAM_FD = 6,
	MAPPLE_PSLIST_STREAM_FD_DETAILS = 7,
	MAPPLE_PSLIST_STREAM_NETWORK = 8,
	MAPPLE_PSLIST_STREAM_OBJECT_END = 9,
	MAPPLE_PSLIST_STREAM_SNAPSHOT_END = 10
};

struct mapple_pslist_stream_record {
	__u32 record_size;
	__u16 record_type;
	__u16 reserved;
	__u64 group;
} PACKED_STRUCT;

enum mapple_pslist_task_type {
	MAPPLE_PSLIST_TASK_THREAD = 1,
	MAPPLE_PSLIST_TASK_PROCESS = 2,
	MAPPLE_PSLIST_TASK_KTHREAD = 3
};

enum mapple_pslist_object_type {
	MAPPLE_PSLIST_OBJECT_MM = 1,
	MAPPLE_PSLIST_OBJECT_FILES = 2
};

enum mapple_pslist_snapshot_status {
	MAPPLE_PSLIST_SNAPSHOT_COMPLETE = 1,
	MAPPLE_PSLIST_SNAPSHOT_PARTIAL = 2
};

enum mapple_pslist_object_status {
	MAPPLE_PSLIST_OBJECT_COMPLETE = 1,
	MAPPLE_PSLIST_OBJECT_PARTIAL = 2,
	MAPPLE_PSLIST_OBJECT_UNAVAILABLE = 3,
	MAPPLE_PSLIST_OBJECT_RACED = 4,
	MAPPLE_PSLIST_OBJECT_TOO_LARGE = 5
};

struct mapple_pslist_snapshot_begin {
	struct mapple_pslist_stream_record record;
	__u64 requested_groups;
	__u64 effective_groups;
	__u64 supported_groups;
	__u64 start_boottime_ns;
} PACKED_STRUCT;

struct mapple_pslist_task_data {
	__u32 pid;
	__u32 tgid;
	__u32 ppid;
	__u64 start_boottime_ns;
	__u8 state;
	__u8 reserved[3];
	char comm[MAPPLE_PSLIST_COMM_SIZE];
} PACKED_STRUCT;

struct mapple_pslist_task_record {
	struct mapple_pslist_stream_record record;
	__u64 task_id;
	__u16 task_type;
	__u16 reserved;
	__u32 reserved2;
	struct mapple_pslist_task_data task;
} PACKED_STRUCT;

struct mapple_pslist_task_object_link {
	struct mapple_pslist_stream_record record;
	__u64 task_id;
	__u64 object_id;
	__u16 object_type;
	__u16 reserved;
	__u32 reserved2;
} PACKED_STRUCT;

struct mapple_pslist_object_begin {
	struct mapple_pslist_stream_record record;
	__u64 object_id;
	__u16 object_type;
	__u16 reserved;
	__u32 reserved2;
} PACKED_STRUCT;

struct mapple_pslist_object_end {
	struct mapple_pslist_stream_record record;
	__u64 object_id;
	__u16 object_type;
	__u16 status;
	__u32 error_code;
	__u32 record_count;
	__u32 reserved;
} PACKED_STRUCT;

struct mapple_pslist_snapshot_end {
	struct mapple_pslist_stream_record record;
	__u16 status;
	__u16 reserved;
	__u32 task_count;
	__u32 object_count;
	__u32 reserved2;
	__u64 end_boottime_ns;
} PACKED_STRUCT;

#define MAPPLE_PSLIST_EXECUTABLE_FLAG_DELETED (1U << 0)
#define MAPPLE_PSLIST_EXECUTABLE_FLAG_PATH_UNAVAILABLE (1U << 1)
#define MAPPLE_PSLIST_EXECUTABLE_FLAG_METADATA_UNAVAILABLE (1U << 2)
#define MAPPLE_PSLIST_EXECUTABLE_KNOWN_FLAGS                                               \
	(MAPPLE_PSLIST_EXECUTABLE_FLAG_DELETED |                                            \
	 MAPPLE_PSLIST_EXECUTABLE_FLAG_PATH_UNAVAILABLE |                                   \
	 MAPPLE_PSLIST_EXECUTABLE_FLAG_METADATA_UNAVAILABLE)

enum mapple_pslist_executable_status {
	MAPPLE_PSLIST_EXECUTABLE_AVAILABLE = 1,
	MAPPLE_PSLIST_EXECUTABLE_NONE = 2,
	MAPPLE_PSLIST_EXECUTABLE_UNAVAILABLE = 3
};

enum mapple_pslist_command_line_status {
	MAPPLE_PSLIST_COMMAND_LINE_AVAILABLE = 1,
	MAPPLE_PSLIST_COMMAND_LINE_EMPTY = 2,
	MAPPLE_PSLIST_COMMAND_LINE_UNAVAILABLE = 3,
	MAPPLE_PSLIST_COMMAND_LINE_TOO_LARGE = 4
};

struct mapple_pslist_process_data {
	__u16 command_line_status;
	__u16 executable_status;
	__u32 executable_flags;
	__u32 command_line_length;
	__u32 executable_path_length;
	__u32 executable_device_major;
	__u32 executable_device_minor;
	__u32 executable_mode;
	__u64 executable_inode;
	__u64 executable_size;
	__s64 executable_mtime_ns;
	__s64 executable_ctime_ns;
	__u64 executable_filesystem_magic;
} PACKED_STRUCT;

struct mapple_pslist_identity_record {
	struct mapple_pslist_stream_record record;
	__u64 object_id;
	struct mapple_pslist_process_data identity;
} PACKED_STRUCT;

enum mapple_pslist_data_status {
	MAPPLE_PSLIST_DATA_AVAILABLE = 1,
	MAPPLE_PSLIST_DATA_UNAVAILABLE = 2,
	MAPPLE_PSLIST_DATA_TOO_LARGE = 3,
	MAPPLE_PSLIST_DATA_RACED = 4,
	MAPPLE_PSLIST_DATA_UNSUPPORTED = 5
};

enum mapple_pslist_fd_kind {
	MAPPLE_PSLIST_FD_REGULAR = 1,
	MAPPLE_PSLIST_FD_DIRECTORY = 2,
	MAPPLE_PSLIST_FD_CHARACTER = 3,
	MAPPLE_PSLIST_FD_BLOCK = 4,
	MAPPLE_PSLIST_FD_PIPE = 5,
	MAPPLE_PSLIST_FD_SOCKET = 6,
	MAPPLE_PSLIST_FD_SYMLINK = 7,
	MAPPLE_PSLIST_FD_ANON_INODE = 8,
	MAPPLE_PSLIST_FD_OTHER = 9
};

#define MAPPLE_PSLIST_FD_DESCRIPTOR_CLOEXEC (1U << 0)
#define MAPPLE_PSLIST_FD_KNOWN_DESCRIPTOR_FLAGS MAPPLE_PSLIST_FD_DESCRIPTOR_CLOEXEC

struct mapple_pslist_fd_record {
	struct mapple_pslist_stream_record record;
	__u64 object_id;
	__u32 fd;
	__u16 kind;
	__u16 path_status;
	__u16 identity_status;
	__u16 reserved;
	__u32 descriptor_flags;
	__u32 open_flags;
	__u32 path_error;
	__u32 identity_error;
	__u32 path_length;
	__u32 device_major;
	__u32 device_minor;
	__u64 inode;
	__u64 mount_id;
} PACKED_STRUCT;

struct mapple_pslist_timespec {
	__s64 seconds;
	__u32 nanoseconds;
	__u32 reserved;
} PACKED_STRUCT;

struct mapple_pslist_fd_stat {
	__u32 result_mask;
	__u32 mode;
	__u32 link_count;
	__u32 block_size;
	__u32 uid;
	__u32 gid;
	__u32 special_device_major;
	__u32 special_device_minor;
	__u32 direct_io_memory_alignment;
	__u32 direct_io_offset_alignment;
	__s64 size;
	__u64 blocks;
	__u64 attributes;
	__u64 attributes_mask;
	__u64 change_cookie;
	__u64 filesystem_magic;
	struct mapple_pslist_timespec access_time;
	struct mapple_pslist_timespec birth_time;
	struct mapple_pslist_timespec change_time;
	struct mapple_pslist_timespec modification_time;
} PACKED_STRUCT;

struct mapple_pslist_fd_details_record {
	struct mapple_pslist_stream_record record;
	__u64 object_id;
	__u32 fd;
	__u16 stat_status;
	__u16 fdinfo_status;
	__u64 file_mode;
	__s64 position;
	__u64 version;
	__u32 credential_uid;
	__u32 credential_gid;
	__u32 credential_effective_uid;
	__u32 credential_effective_gid;
	__u32 credential_saved_uid;
	__u32 credential_saved_gid;
	__u32 credential_filesystem_uid;
	__u32 credential_filesystem_gid;
	__s32 async_owner_pid;
	__u16 async_owner_type;
	__u16 async_signal;
	__u32 async_owner_uid;
	__u32 async_owner_effective_uid;
	__u32 stat_error;
	__u32 fdinfo_error;
	__u32 fdinfo_length;
	__u32 reserved;
	struct mapple_pslist_fd_stat stat;
} PACKED_STRUCT;

enum mapple_pslist_socket_endpoint_type {
	MAPPLE_PSLIST_SOCKET_ENDPOINT_NONE = 0,
	MAPPLE_PSLIST_SOCKET_ENDPOINT_IPV4 = 1,
	MAPPLE_PSLIST_SOCKET_ENDPOINT_IPV6 = 2,
	MAPPLE_PSLIST_SOCKET_ENDPOINT_UNIX = 3,
	MAPPLE_PSLIST_SOCKET_ENDPOINT_NETLINK = 4
};

enum mapple_pslist_socket_endpoint_status {
	MAPPLE_PSLIST_SOCKET_ENDPOINT_AVAILABLE = 1,
	MAPPLE_PSLIST_SOCKET_ENDPOINT_UNAVAILABLE = 2,
	MAPPLE_PSLIST_SOCKET_ENDPOINT_NOT_CONNECTED = 3,
	MAPPLE_PSLIST_SOCKET_ENDPOINT_UNSUPPORTED = 4
};

#define MAPPLE_PSLIST_SOCKET_ENDPOINT_ABSTRACT (1U << 0)
#define MAPPLE_PSLIST_SOCKET_ENDPOINT_KNOWN_FLAGS MAPPLE_PSLIST_SOCKET_ENDPOINT_ABSTRACT

struct mapple_pslist_socket_endpoint {
	__u16 status;
	__u16 type;
	__s32 error;
	__u16 port;
	__u16 flags;
	__u32 scope_id;
	__u32 flow_info;
	__u32 netlink_pid;
	__u32 netlink_groups;
	__u32 address_length;
} PACKED_STRUCT;

struct mapple_pslist_socket_data {
	__u16 family;
	__u16 type;
	__u16 protocol;
	__u16 socket_state;
	__u16 protocol_state;
	__u16 shutdown;
	__s32 error;
	__s32 soft_error;
	__u32 bound_device;
	__u32 mark;
	__u32 priority;
	__u32 uid;
	__u32 network_namespace;
	__u32 receive_buffer;
	__u32 send_buffer;
	__u32 receive_queued;
	__u32 send_queued;
	__u32 drops;
	struct mapple_pslist_socket_endpoint local;
	struct mapple_pslist_socket_endpoint peer;
} PACKED_STRUCT;

struct mapple_pslist_network_record {
	struct mapple_pslist_stream_record record;
	__u64 object_id;
	__u32 fd;
	__u32 local_address_length;
	__u32 peer_address_length;
	__u32 reserved;
	struct mapple_pslist_socket_data socket;
} PACKED_STRUCT;

#undef PACKED_STRUCT

#endif /* MAPPLE_COMMANDS_H */
