#ifndef COMMAND_PSLIST_MODEL_H
#define COMMAND_PSLIST_MODEL_H

#include <stddef.h>

#include <commands.h>

enum mapple_pslist_output_group {
	MAPPLE_PSLIST_OUTPUT_IDENTITY,
	MAPPLE_PSLIST_OUTPUT_SECURITY,
	MAPPLE_PSLIST_OUTPUT_CONTEXT,
	MAPPLE_PSLIST_OUTPUT_RESOURCES,
	MAPPLE_PSLIST_OUTPUT_FDS,
	MAPPLE_PSLIST_OUTPUT_NETWORK,
	MAPPLE_PSLIST_OUTPUT_GROUP_COUNT,
};

struct mapple_pslist_task {
	__u64 group_object_ids[MAPPLE_PSLIST_OUTPUT_GROUP_COUNT];
	__u32 pid;
	__u32 tgid;
	__u32 ppid;
	__u64 start_boottime_ns;
	__u16 type;
	__u8 state;
	char comm[MAPPLE_PSLIST_COMM_SIZE];
};

struct mapple_pslist_time {
	__s64 seconds;
	__u32 nanoseconds;
};

struct mapple_pslist_stat {
	__u32 result_mask;
	__u32 mode;
	__u32 link_count;
	__u32 block_size;
	__u32 uid;
	__u32 gid;
	__u32 device_major;
	__u32 device_minor;
	__u32 special_device_major;
	__u32 special_device_minor;
	__u32 direct_io_memory_alignment;
	__u32 direct_io_offset_alignment;
	__u64 inode;
	__s64 size;
	__u64 blocks;
	__u64 attributes;
	__u64 attributes_mask;
	__u64 mount_id;
	__u64 change_cookie;
	__u64 filesystem_magic;
	struct mapple_pslist_time access_time;
	struct mapple_pslist_time birth_time;
	struct mapple_pslist_time change_time;
	struct mapple_pslist_time modification_time;
};

struct mapple_pslist_socket_endpoint_data {
	__u16 status;
	__u16 type;
	__s32 error;
	__u16 port;
	__u16 flags;
	__u32 scope_id;
	__u32 flow_info;
	__u32 netlink_pid;
	__u32 netlink_groups;
	unsigned char *address;
	__u32 address_length;
};

struct mapple_pslist_socket {
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
	struct mapple_pslist_socket_endpoint_data local;
	struct mapple_pslist_socket_endpoint_data peer;
};

struct mapple_pslist_fd {
	__u32 fd;
	__u16 kind;
	__u32 descriptor_flags;
	__u32 open_flags;
	__u16 path_status;
	__u16 identity_status;
	__s32 path_error;
	__s32 identity_error;
	unsigned char *path;
	__u32 path_length;
	int details_available;
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
	__u16 stat_status;
	__u16 fdinfo_status;
	__s32 stat_error;
	__s32 fdinfo_error;
	struct mapple_pslist_stat stat;
	unsigned char *fdinfo;
	__u32 fdinfo_length;
};

struct mapple_pslist_fd_table {
	__u64 object_id;
	struct mapple_pslist_fd *entries;
	size_t count;
	size_t capacity;
	__u16 fds_status;
	int has_fds;
	int has_details;
};

struct mapple_pslist_network_entry {
	__u32 fd;
	struct mapple_pslist_socket socket;
};

struct mapple_pslist_network_table {
	__u64 object_id;
	struct mapple_pslist_network_entry *entries;
	size_t count;
	size_t capacity;
	__u16 status;
	int has_records;
};

struct mapple_pslist_identity {
	__u64 object_id;
	__u16 status;
	int has_record;
	__u16 command_line_status;
	__u16 executable_status;
	__u32 executable_flags;
	__u32 executable_device_major;
	__u32 executable_device_minor;
	__u32 executable_mode;
	unsigned char *command_line;
	__u32 command_line_length;
	unsigned char *executable_path;
	__u32 executable_path_length;
	__u64 executable_inode;
	__u64 executable_size;
	__s64 executable_mtime_ns;
	__s64 executable_ctime_ns;
	__u64 executable_filesystem_magic;
};

struct mapple_pslist_security {
	__u64 object_id;
	__u16 status;
	int has_record;
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
	__s32 lsm_error;
	__u32 lsm_secid;
	__s32 lsm_provider_error;
	__u64 capability_inheritable;
	__u64 capability_permitted;
	__u64 capability_effective;
	__u64 capability_bounding;
	__u64 capability_ambient;
	__u32 *supplementary_groups;
	__u32 supplementary_group_count;
	unsigned char *lsm_provider;
	__u32 lsm_provider_length;
	unsigned char *lsm_context;
	__u32 lsm_context_length;
};

struct mapple_pslist_context {
	__u64 object_id;
	__u16 status;
	int has_record;
	__u16 cwd_status;
	__u16 root_status;
	__u16 cgroup_status;
	__u16 cgroup_host_path_status;
	__u16 cgroup_namespace_path_status;
	__s32 cwd_error;
	__s32 root_error;
	__s32 cgroup_error;
	__s32 cgroup_host_path_error;
	__s32 cgroup_namespace_path_error;
	unsigned char *cwd;
	unsigned char *root;
	unsigned char *cgroup_host_path;
	unsigned char *cgroup_namespace_path;
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
};

struct mapple_pslist_resources {
	__u64 object_id;
	__u16 status;
	int has_record;
	__u16 memory_status;
	__u16 io_status;
	__u16 network_status;
	__u16 memory_reason;
	__s32 memory_error;
	__s32 io_error;
	__s32 network_error;
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
};

struct mapple_pslist_snapshot {
	struct mapple_pslist_task *tasks;
	size_t task_count;
	size_t task_capacity;
	struct mapple_pslist_identity *identities;
	size_t identity_count;
	size_t identity_capacity;
	struct mapple_pslist_security *security;
	size_t security_count;
	size_t security_capacity;
	struct mapple_pslist_context *contexts;
	size_t context_count;
	size_t context_capacity;
	struct mapple_pslist_resources *resources;
	size_t resources_count;
	size_t resources_capacity;
	struct mapple_pslist_fd_table *fd_tables;
	size_t fd_table_count;
	size_t fd_table_capacity;
	struct mapple_pslist_network_table *network_tables;
	size_t network_table_count;
	size_t network_table_capacity;
	__u64 requested_groups;
	__u64 effective_groups;
	__u64 supported_groups;
	__u64 start_boottime_ns;
	__u64 end_boottime_ns;
	__u16 status;
};

#endif /* COMMAND_PSLIST_MODEL_H */
