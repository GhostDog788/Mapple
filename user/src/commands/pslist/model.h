#ifndef COMMAND_PSLIST_MODEL_H
#define COMMAND_PSLIST_MODEL_H

#include <stddef.h>

#include <commands.h>

struct mapple_pslist_task {
	__u64 task_id;
	__u64 identity_object_id;
	__u64 fds_object_id;
	__u64 fd_details_object_id;
	__u64 network_object_id;
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
	int network_available;
	struct mapple_pslist_socket socket;
};

struct mapple_pslist_fd_table {
	__u64 object_id;
	struct mapple_pslist_fd *entries;
	size_t count;
	size_t capacity;
	__u16 fds_status;
	__u16 details_status;
	__u16 network_status;
	int has_fds;
	int has_details;
	int has_network;
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

struct mapple_pslist_snapshot {
	struct mapple_pslist_task *tasks;
	size_t task_count;
	size_t task_capacity;
	struct mapple_pslist_identity *identities;
	size_t identity_count;
	size_t identity_capacity;
	struct mapple_pslist_fd_table *fd_tables;
	size_t fd_table_count;
	size_t fd_table_capacity;
	__u64 requested_groups;
	__u64 effective_groups;
	__u64 supported_groups;
	__u64 start_boottime_ns;
	__u64 end_boottime_ns;
	__u16 status;
};

#endif /* COMMAND_PSLIST_MODEL_H */
