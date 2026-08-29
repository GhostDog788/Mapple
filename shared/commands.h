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
#define MAPPLE_PSLIST_ENVIRONMENT_MAX_SIZE (6U * 1024U * 1024U)
#define MAPPLE_PSLIST_FDINFO_MAX_SIZE (6U * 1024U * 1024U)
#define MAPPLE_PSLIST_CONTEXT_PATH_MAX_SIZE 4096U
#define MAPPLE_PSLIST_MEMORY_MAP_MAX_COUNT 262144U
#define MAPPLE_PSLIST_MEMORY_MAP_NAME_MAX_SIZE 256U
#define MAPPLE_PSLIST_MEMORY_MAP_PATH_MAX_SIZE 4096U
#define MAPPLE_PSLIST_ELF_STRING_MAX_SIZE 4096U
#define MAPPLE_PSLIST_ELF_BUILD_ID_MAX_SIZE 128U
#define MAPPLE_PSLIST_ELF_MAX_PROGRAM_HEADERS 4096U
#define MAPPLE_PSLIST_ELF_MAX_SECTION_HEADERS 65536U
#define MAPPLE_PSLIST_ELF_MAX_DYNAMIC_STRINGS 65536U
#define MAPPLE_PSLIST_ELF_MAX_DYNAMIC_SYMBOLS 262144U
#define MAPPLE_PSLIST_ELF_MAX_TABLE_SIZE (16U * 1024U * 1024U)

#define MAPPLE_PSLIST_GROUP_IDENTITY (1ULL << 0)
#define MAPPLE_PSLIST_GROUP_ENVIRONMENT (1ULL << 1)
#define MAPPLE_PSLIST_GROUP_SECURITY (1ULL << 2)
#define MAPPLE_PSLIST_GROUP_CONTEXT (1ULL << 3)
#define MAPPLE_PSLIST_GROUP_RESOURCES (1ULL << 4)
#define MAPPLE_PSLIST_GROUP_FDS (1ULL << 5)
#define MAPPLE_PSLIST_GROUP_FDS_DETAILS (1ULL << 6)
#define MAPPLE_PSLIST_GROUP_NETWORK (1ULL << 7)
#define MAPPLE_PSLIST_GROUP_MEMORY_MAPS (1ULL << 8)
#define MAPPLE_PSLIST_GROUP_ELF (1ULL << 9)

#define MAPPLE_PSLIST_KNOWN_GROUPS                                                   \
	(MAPPLE_PSLIST_GROUP_IDENTITY | MAPPLE_PSLIST_GROUP_ENVIRONMENT |             \
	 MAPPLE_PSLIST_GROUP_SECURITY | MAPPLE_PSLIST_GROUP_CONTEXT |                 \
	 MAPPLE_PSLIST_GROUP_RESOURCES | MAPPLE_PSLIST_GROUP_FDS |                    \
	 MAPPLE_PSLIST_GROUP_FDS_DETAILS | MAPPLE_PSLIST_GROUP_NETWORK |              \
	 MAPPLE_PSLIST_GROUP_MEMORY_MAPS | MAPPLE_PSLIST_GROUP_ELF)

#define MAPPLE_PSLIST_SUPPORTED_GROUPS                                               \
	(MAPPLE_PSLIST_GROUP_IDENTITY | MAPPLE_PSLIST_GROUP_ENVIRONMENT |              \
	 MAPPLE_PSLIST_GROUP_SECURITY | MAPPLE_PSLIST_GROUP_CONTEXT |                 \
	 MAPPLE_PSLIST_GROUP_RESOURCES | MAPPLE_PSLIST_GROUP_FDS |                    \
	 MAPPLE_PSLIST_GROUP_FDS_DETAILS | MAPPLE_PSLIST_GROUP_NETWORK |              \
	 MAPPLE_PSLIST_GROUP_MEMORY_MAPS | MAPPLE_PSLIST_GROUP_ELF)

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
	MAPPLE_PSLIST_STREAM_SNAPSHOT_END = 10,
	MAPPLE_PSLIST_STREAM_SECURITY = 11,
	MAPPLE_PSLIST_STREAM_CONTEXT = 12,
	MAPPLE_PSLIST_STREAM_RESOURCES = 13,
	MAPPLE_PSLIST_STREAM_ENVIRONMENT = 14,
	MAPPLE_PSLIST_STREAM_MEMORY_MAPS_METADATA = 15,
	MAPPLE_PSLIST_STREAM_MEMORY_MAP = 16,
	MAPPLE_PSLIST_STREAM_ELF_METADATA = 17,
	MAPPLE_PSLIST_STREAM_ELF_STRING = 18,
	MAPPLE_PSLIST_STREAM_ELF_SYMBOL = 19
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
	MAPPLE_PSLIST_OBJECT_FILES = 2,
	MAPPLE_PSLIST_OBJECT_TASK = 3,
	MAPPLE_PSLIST_OBJECT_ELF_FILE = 4
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
	__u16 task_type;
	__u16 reserved;
	struct mapple_pslist_task_data task;
} PACKED_STRUCT;

struct mapple_pslist_task_object_link {
	struct mapple_pslist_stream_record record;
	__u32 pid;
	__u16 object_type;
	__u16 reserved;
	__u64 object_id;
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

#define MAPPLE_PSLIST_SECURITY_MAX_SUPPLEMENTARY_GROUPS 65536U
#define MAPPLE_PSLIST_LSM_PROVIDER_MAX_SIZE 64U

enum mapple_pslist_dumpable_reason {
	MAPPLE_PSLIST_DUMPABLE_REASON_NONE = 0,
	MAPPLE_PSLIST_DUMPABLE_REASON_NO_MM = 1
};

struct mapple_pslist_security_record {
	struct mapple_pslist_stream_record record;
	__u64 object_id;
	__u32 uid;
	__u32 gid;
	__u32 effective_uid;
	__u32 effective_gid;
	__u32 saved_uid;
	__u32 saved_gid;
	__u32 filesystem_uid;
	__u32 filesystem_gid;
	__u32 user_namespace;
	__u32 securebits;
	__u32 no_new_privs;
	__u32 seccomp_mode;
	__u32 seccomp_filter_count;
	__u16 dumpable_status;
	__u16 dumpable_reason;
	__u16 lsm_status;
	__u16 lsm_provider_status;
	__u32 dumpable;
	__u32 lsm_error;
	__u32 lsm_secid;
	__u32 lsm_provider_error;
	__u32 supplementary_group_count;
	__u32 lsm_provider_length;
	__u32 lsm_context_length;
	__u32 reserved;
	__u64 capability_inheritable;
	__u64 capability_permitted;
	__u64 capability_effective;
	__u64 capability_bounding;
	__u64 capability_ambient;
} PACKED_STRUCT;
/* Followed by supplementary GIDs, LSM provider bytes, then LSM context bytes. */

#define MAPPLE_PSLIST_NAMESPACE_USER (1U << 0)
#define MAPPLE_PSLIST_NAMESPACE_UTS (1U << 1)
#define MAPPLE_PSLIST_NAMESPACE_IPC (1U << 2)
#define MAPPLE_PSLIST_NAMESPACE_MOUNT (1U << 3)
#define MAPPLE_PSLIST_NAMESPACE_PID_ACTIVE (1U << 4)
#define MAPPLE_PSLIST_NAMESPACE_PID_CHILDREN (1U << 5)
#define MAPPLE_PSLIST_NAMESPACE_NETWORK (1U << 6)
#define MAPPLE_PSLIST_NAMESPACE_CGROUP (1U << 7)
#define MAPPLE_PSLIST_NAMESPACE_TIME (1U << 8)
#define MAPPLE_PSLIST_NAMESPACE_TIME_CHILDREN (1U << 9)
#define MAPPLE_PSLIST_NAMESPACE_KNOWN_MASK                                         \
	(MAPPLE_PSLIST_NAMESPACE_USER | MAPPLE_PSLIST_NAMESPACE_UTS |                 \
	 MAPPLE_PSLIST_NAMESPACE_IPC | MAPPLE_PSLIST_NAMESPACE_MOUNT |               \
	 MAPPLE_PSLIST_NAMESPACE_PID_ACTIVE |                                        \
	 MAPPLE_PSLIST_NAMESPACE_PID_CHILDREN | MAPPLE_PSLIST_NAMESPACE_NETWORK |    \
	 MAPPLE_PSLIST_NAMESPACE_CGROUP | MAPPLE_PSLIST_NAMESPACE_TIME |             \
	 MAPPLE_PSLIST_NAMESPACE_TIME_CHILDREN)

struct mapple_pslist_context_record {
	struct mapple_pslist_stream_record record;
	__u64 object_id;
	__u16 cwd_status;
	__u16 root_status;
	__u16 cgroup_status;
	__u16 cgroup_host_path_status;
	__u16 cgroup_namespace_path_status;
	__u16 reserved;
	__u32 cwd_error;
	__u32 root_error;
	__u32 cgroup_error;
	__u32 cgroup_host_path_error;
	__u32 cgroup_namespace_path_error;
	__u32 cwd_length;
	__u32 root_length;
	__u32 cgroup_host_path_length;
	__u32 cgroup_namespace_path_length;
	__u32 namespace_available_mask;
	__u32 namespace_unsupported_mask;
	__u32 user_namespace;
	__u32 uts_namespace;
	__u32 ipc_namespace;
	__u32 pid_namespace_active;
	__u32 pid_namespace_children;
	__u32 network_namespace;
	__u32 cgroup_namespace;
	__u32 time_namespace;
	__u32 time_namespace_children;
	__u32 cgroup_hierarchy_id;
	__u64 cgroup_id;
	__u64 cgroup_inode;
} PACKED_STRUCT;
/* Followed by cwd, root, host cgroup path, then namespaced cgroup path bytes. */

enum mapple_pslist_resources_memory_reason {
	MAPPLE_PSLIST_RESOURCES_MEMORY_REASON_NONE = 0,
	MAPPLE_PSLIST_RESOURCES_MEMORY_REASON_NO_MM = 1
};

struct mapple_pslist_resources_record {
	struct mapple_pslist_stream_record record;
	__u64 object_id;
	__u16 memory_status;
	__u16 io_status;
	__u16 network_status;
	__u16 memory_reason;
	__u32 memory_error;
	__u32 io_error;
	__u32 network_error;
	__u32 current_cpu;
	__u32 scheduling_policy;
	__s32 priority;
	__s32 nice;
	__u32 realtime_priority;
	__u32 page_size_bytes;
	__u64 cpu_user_time_ns;
	__u64 cpu_system_time_ns;
	__u64 minor_faults;
	__u64 major_faults;
	__u64 voluntary_context_switches;
	__u64 involuntary_context_switches;
	__u64 io_read_characters;
	__u64 io_write_characters;
	__u64 io_read_syscalls;
	__u64 io_write_syscalls;
	__u64 io_storage_read_bytes;
	__u64 io_storage_write_bytes;
	__u64 io_cancelled_write_bytes;
	__u64 memory_virtual_pages;
	__u64 memory_peak_virtual_pages;
	__u64 memory_locked_pages;
	__u64 memory_data_pages;
	__u64 memory_stack_pages;
	__u64 memory_executable_pages;
	__u64 memory_resident_pages;
	__u64 memory_peak_resident_pages;
	__u64 memory_anonymous_pages;
	__u64 memory_file_pages;
	__u64 memory_shared_pages;
	__u64 memory_swap_entries;
} PACKED_STRUCT;

#define MAPPLE_PSLIST_ENVIRONMENT_FLAG_TERMINATED (1U << 0)
#define MAPPLE_PSLIST_ENVIRONMENT_KNOWN_FLAGS                                      \
	MAPPLE_PSLIST_ENVIRONMENT_FLAG_TERMINATED

enum mapple_pslist_environment_status {
	MAPPLE_PSLIST_ENVIRONMENT_AVAILABLE = 1,
	MAPPLE_PSLIST_ENVIRONMENT_EMPTY = 2,
	MAPPLE_PSLIST_ENVIRONMENT_UNAVAILABLE = 3,
	MAPPLE_PSLIST_ENVIRONMENT_TOO_LARGE = 4,
	MAPPLE_PSLIST_ENVIRONMENT_RACED = 5
};

struct mapple_pslist_environment_record {
	struct mapple_pslist_stream_record record;
	__u64 object_id;
	__u16 status;
	__u16 flags;
	__u32 error;
	__u32 byte_length;
	__u32 entry_count;
	__u32 reserved;
} PACKED_STRUCT;
/* Followed by the raw NUL-delimited environment block. */

enum mapple_pslist_memory_mapping_type {
	MAPPLE_PSLIST_MEMORY_MAPPING_ANONYMOUS = 1,
	MAPPLE_PSLIST_MEMORY_MAPPING_FILE = 2,
	MAPPLE_PSLIST_MEMORY_MAPPING_SPECIAL = 3
};

enum mapple_pslist_memory_mapping_name_kind {
	MAPPLE_PSLIST_MEMORY_MAP_NAME_NONE = 0,
	MAPPLE_PSLIST_MEMORY_MAP_NAME_HEAP = 1,
	MAPPLE_PSLIST_MEMORY_MAP_NAME_STACK = 2,
	MAPPLE_PSLIST_MEMORY_MAP_NAME_ANONYMOUS = 3,
	MAPPLE_PSLIST_MEMORY_MAP_NAME_SHARED_ANONYMOUS = 4,
	MAPPLE_PSLIST_MEMORY_MAP_NAME_SPECIAL = 5
};

#define MAPPLE_PSLIST_MEMORY_MAP_FILE_DELETED (1U << 0)
#define MAPPLE_PSLIST_MEMORY_MAP_KNOWN_FILE_FLAGS MAPPLE_PSLIST_MEMORY_MAP_FILE_DELETED

#define MAPPLE_PSLIST_VM_READ (1ULL << 0)
#define MAPPLE_PSLIST_VM_WRITE (1ULL << 1)
#define MAPPLE_PSLIST_VM_EXEC (1ULL << 2)
#define MAPPLE_PSLIST_VM_SHARED (1ULL << 3)
#define MAPPLE_PSLIST_VM_MAYREAD (1ULL << 4)
#define MAPPLE_PSLIST_VM_MAYWRITE (1ULL << 5)
#define MAPPLE_PSLIST_VM_MAYEXEC (1ULL << 6)
#define MAPPLE_PSLIST_VM_MAYSHARE (1ULL << 7)
#define MAPPLE_PSLIST_VM_GROWSDOWN (1ULL << 8)
#define MAPPLE_PSLIST_VM_UFFD_MISSING (1ULL << 9)
#define MAPPLE_PSLIST_VM_PFNMAP (1ULL << 10)
#define MAPPLE_PSLIST_VM_UFFD_WP (1ULL << 12)
#define MAPPLE_PSLIST_VM_LOCKED (1ULL << 13)
#define MAPPLE_PSLIST_VM_IO (1ULL << 14)
#define MAPPLE_PSLIST_VM_SEQ_READ (1ULL << 15)
#define MAPPLE_PSLIST_VM_RAND_READ (1ULL << 16)
#define MAPPLE_PSLIST_VM_DONTCOPY (1ULL << 17)
#define MAPPLE_PSLIST_VM_DONTEXPAND (1ULL << 18)
#define MAPPLE_PSLIST_VM_LOCKONFAULT (1ULL << 19)
#define MAPPLE_PSLIST_VM_ACCOUNT (1ULL << 20)
#define MAPPLE_PSLIST_VM_NORESERVE (1ULL << 21)
#define MAPPLE_PSLIST_VM_HUGETLB (1ULL << 22)
#define MAPPLE_PSLIST_VM_SYNC (1ULL << 23)
#define MAPPLE_PSLIST_VM_WIPEONFORK (1ULL << 25)
#define MAPPLE_PSLIST_VM_DONTDUMP (1ULL << 26)
#define MAPPLE_PSLIST_VM_SOFTDIRTY (1ULL << 27)
#define MAPPLE_PSLIST_VM_MIXEDMAP (1ULL << 28)
#define MAPPLE_PSLIST_VM_HUGEPAGE (1ULL << 29)
#define MAPPLE_PSLIST_VM_NOHUGEPAGE (1ULL << 30)
#define MAPPLE_PSLIST_VM_MERGEABLE (1ULL << 31)

#define MAPPLE_PSLIST_VM_KNOWN_FLAGS                                                \
	(MAPPLE_PSLIST_VM_READ | MAPPLE_PSLIST_VM_WRITE | MAPPLE_PSLIST_VM_EXEC |    \
	 MAPPLE_PSLIST_VM_SHARED | MAPPLE_PSLIST_VM_MAYREAD |                       \
	 MAPPLE_PSLIST_VM_MAYWRITE | MAPPLE_PSLIST_VM_MAYEXEC |                     \
	 MAPPLE_PSLIST_VM_MAYSHARE | MAPPLE_PSLIST_VM_GROWSDOWN |                   \
	 MAPPLE_PSLIST_VM_UFFD_MISSING | MAPPLE_PSLIST_VM_PFNMAP |                  \
	 MAPPLE_PSLIST_VM_UFFD_WP | MAPPLE_PSLIST_VM_LOCKED | MAPPLE_PSLIST_VM_IO | \
	 MAPPLE_PSLIST_VM_SEQ_READ | MAPPLE_PSLIST_VM_RAND_READ |                   \
	 MAPPLE_PSLIST_VM_DONTCOPY | MAPPLE_PSLIST_VM_DONTEXPAND |                  \
	 MAPPLE_PSLIST_VM_LOCKONFAULT | MAPPLE_PSLIST_VM_ACCOUNT |                  \
	 MAPPLE_PSLIST_VM_NORESERVE | MAPPLE_PSLIST_VM_HUGETLB |                   \
	 MAPPLE_PSLIST_VM_SYNC | MAPPLE_PSLIST_VM_WIPEONFORK |                      \
	 MAPPLE_PSLIST_VM_DONTDUMP | MAPPLE_PSLIST_VM_SOFTDIRTY |                   \
	 MAPPLE_PSLIST_VM_MIXEDMAP | MAPPLE_PSLIST_VM_HUGEPAGE |                    \
	 MAPPLE_PSLIST_VM_NOHUGEPAGE | MAPPLE_PSLIST_VM_MERGEABLE)

struct mapple_pslist_memory_maps_metadata_record {
	struct mapple_pslist_stream_record record;
	__u64 object_id;
	__u32 page_size_bytes;
	__u32 mapping_count;
	__u32 mapping_limit;
	__u32 reserved;
} PACKED_STRUCT;

struct mapple_pslist_memory_map_record {
	struct mapple_pslist_stream_record record;
	__u64 object_id;
	__u64 elf_object_id;
	__u32 mapping_index;
	__u16 mapping_type;
	__u16 name_kind;
	__u16 name_status;
	__u16 path_status;
	__u16 file_metadata_status;
	__u16 reserved;
	__u32 name_error;
	__u32 path_error;
	__u32 file_metadata_error;
	__u32 file_flags;
	__u32 name_length;
	__u32 path_length;
	__u32 file_device_major;
	__u32 file_device_minor;
	__u32 file_mode;
	__u32 protection_key;
	__u64 start_address;
	__u64 end_address;
	__u64 vm_flags;
	__u64 page_offset;
	__u64 mapping_page_size;
	__u64 file_inode;
	__u64 file_size;
	__s64 file_mtime_ns;
	__s64 file_ctime_ns;
	__u64 file_filesystem_magic;
} PACKED_STRUCT;
/* Followed by mapping-name bytes, then mapped-file path bytes. */

enum mapple_pslist_elf_string_kind {
	MAPPLE_PSLIST_ELF_STRING_INTERPRETER = 1,
	MAPPLE_PSLIST_ELF_STRING_NEEDED = 2,
	MAPPLE_PSLIST_ELF_STRING_SONAME = 3,
	MAPPLE_PSLIST_ELF_STRING_RPATH = 4,
	MAPPLE_PSLIST_ELF_STRING_RUNPATH = 5,
	MAPPLE_PSLIST_ELF_STRING_BUILD_ID = 6
};

enum mapple_pslist_elf_symbol_kind {
	MAPPLE_PSLIST_ELF_SYMBOL_IMPORT = 1,
	MAPPLE_PSLIST_ELF_SYMBOL_EXPORT = 2
};

struct mapple_pslist_elf_metadata_record {
	struct mapple_pslist_stream_record record;
	__u64 object_id;
	__u8 elf_class;
	__u8 data_encoding;
	__u8 os_abi;
	__u8 abi_version;
	__u16 object_type;
	__u16 machine;
	__u32 elf_version;
	__u32 flags;
	__u32 program_header_count;
	__u32 section_header_count;
	__u32 string_record_count;
	__u32 symbol_record_count;
	__u32 import_count;
	__u32 export_count;
	__u64 entry_address;
	__u64 program_header_offset;
	__u64 section_header_offset;
	__u16 program_header_entry_size;
	__u16 section_header_entry_size;
	__u16 section_name_index;
	__u16 reserved;
	__u32 file_device_major;
	__u32 file_device_minor;
	__u32 file_mode;
	__u32 reserved2;
	__u64 file_inode;
	__u64 file_size;
	__s64 file_mtime_ns;
	__s64 file_ctime_ns;
	__u64 file_filesystem_magic;
} PACKED_STRUCT;

struct mapple_pslist_elf_string_record {
	struct mapple_pslist_stream_record record;
	__u64 object_id;
	__u32 string_index;
	__u16 string_kind;
	__u16 reserved;
	__u32 byte_length;
	__u32 reserved2;
} PACKED_STRUCT;
/* Followed by byte_length bytes. Build IDs are binary; all other kinds are text. */

struct mapple_pslist_elf_symbol_record {
	struct mapple_pslist_stream_record record;
	__u64 object_id;
	__u32 symbol_index;
	__u16 symbol_kind;
	__u8 binding;
	__u8 symbol_type;
	__u8 visibility;
	__u8 reserved;
	__u16 section_index;
	__u32 name_length;
	__u32 reserved2;
	__u64 value;
	__u64 size;
} PACKED_STRUCT;
/* Followed by name_length bytes. */

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
