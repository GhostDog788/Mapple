#define _GNU_SOURCE

#include <commands/pslist/fd.h>

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <communication/stream.h>
#include <commands/pslist/json.h>

#define MAPPLE_PSLIST_FD_INITIAL_CAPACITY 16U
#define MAPPLE_PSLIST_FD_SKIP_BUFFER_SIZE 4096U

static __u16 mapple_pslist_fd_read_u16(const void *record, size_t offset)
{
	__u16 value;

	memcpy(&value, (const unsigned char *)record + offset, sizeof(value));
	return ntohs(value);
}

static __u32 mapple_pslist_fd_read_u32(const void *record, size_t offset)
{
	__u32 value;

	memcpy(&value, (const unsigned char *)record + offset, sizeof(value));
	return ntohl(value);
}

static __u64 mapple_pslist_fd_read_u64(const void *record, size_t offset)
{
	__u64 value;

	memcpy(&value, (const unsigned char *)record + offset, sizeof(value));
	return be64toh(value);
}

static int mapple_pslist_fd_skip(int session_fd, size_t size)
{
	unsigned char buffer[MAPPLE_PSLIST_FD_SKIP_BUFFER_SIZE];
	int ret;

	while (size) {
		size_t chunk = size < sizeof(buffer) ? size : sizeof(buffer);

		ret = read_exact(session_fd, buffer, chunk);
		if (ret)
			return ret;
		size -= chunk;
	}
	return 0;
}

static int mapple_pslist_fd_grow(struct mapple_pslist_fd_table *table)
{
	struct mapple_pslist_fd *new_entries;
	size_t new_capacity;

	if (table->count < table->capacity)
		return 0;
	if (!table->capacity)
		new_capacity = MAPPLE_PSLIST_FD_INITIAL_CAPACITY;
	else {
		if (table->capacity > SIZE_MAX / 2)
			return EOVERFLOW;
		new_capacity = table->capacity * 2;
	}
	if (new_capacity > SIZE_MAX / sizeof(*table->entries))
		return EOVERFLOW;
	new_entries = realloc(table->entries, new_capacity * sizeof(*table->entries));
	if (!new_entries)
		return ENOMEM;
	table->entries = new_entries;
	table->capacity = new_capacity;
	return 0;
}

static void mapple_pslist_free_fd(struct mapple_pslist_fd *fd)
{
	free(fd->socket.local.address);
	free(fd->socket.peer.address);
	free(fd->path);
	free(fd->fdinfo);
}

void mapple_pslist_free_fd_table(struct mapple_pslist_fd_table *table)
{
	size_t index;

	for (index = 0; index < table->count; ++index)
		mapple_pslist_free_fd(&table->entries[index]);
	free(table->entries);
	memset(table, 0, sizeof(*table));
}

static void mapple_pslist_decode_time(const struct mapple_pslist_timespec *wire,
				      struct mapple_pslist_time *time)
{
	time->seconds = (__s64)mapple_pslist_fd_read_u64(
		wire, offsetof(struct mapple_pslist_timespec, seconds));
	time->nanoseconds = mapple_pslist_fd_read_u32(
		wire, offsetof(struct mapple_pslist_timespec, nanoseconds));
}

static int mapple_pslist_decode_stat(const struct mapple_pslist_fd_stat *wire,
				     struct mapple_pslist_stat *stat)
{
	if (wire->access_time.reserved || wire->birth_time.reserved ||
	    wire->change_time.reserved || wire->modification_time.reserved)
		return EPROTO;
	stat->result_mask = mapple_pslist_fd_read_u32(
		wire, offsetof(struct mapple_pslist_fd_stat, result_mask));
	stat->mode = mapple_pslist_fd_read_u32(wire, offsetof(struct mapple_pslist_fd_stat, mode));
	stat->link_count = mapple_pslist_fd_read_u32(
		wire, offsetof(struct mapple_pslist_fd_stat, link_count));
	stat->block_size = mapple_pslist_fd_read_u32(
		wire, offsetof(struct mapple_pslist_fd_stat, block_size));
	stat->uid = mapple_pslist_fd_read_u32(wire, offsetof(struct mapple_pslist_fd_stat, uid));
	stat->gid = mapple_pslist_fd_read_u32(wire, offsetof(struct mapple_pslist_fd_stat, gid));
	stat->device_major = mapple_pslist_fd_read_u32(
		wire, offsetof(struct mapple_pslist_fd_stat, device_major));
	stat->device_minor = mapple_pslist_fd_read_u32(
		wire, offsetof(struct mapple_pslist_fd_stat, device_minor));
	stat->special_device_major = mapple_pslist_fd_read_u32(
		wire, offsetof(struct mapple_pslist_fd_stat, special_device_major));
	stat->special_device_minor = mapple_pslist_fd_read_u32(
		wire, offsetof(struct mapple_pslist_fd_stat, special_device_minor));
	stat->direct_io_memory_alignment = mapple_pslist_fd_read_u32(
		wire, offsetof(struct mapple_pslist_fd_stat, direct_io_memory_alignment));
	stat->direct_io_offset_alignment = mapple_pslist_fd_read_u32(
		wire, offsetof(struct mapple_pslist_fd_stat, direct_io_offset_alignment));
	stat->inode = mapple_pslist_fd_read_u64(wire, offsetof(struct mapple_pslist_fd_stat, inode));
	stat->size = (__s64)mapple_pslist_fd_read_u64(
		wire, offsetof(struct mapple_pslist_fd_stat, size));
	stat->blocks = mapple_pslist_fd_read_u64(wire, offsetof(struct mapple_pslist_fd_stat, blocks));
	stat->attributes = mapple_pslist_fd_read_u64(
		wire, offsetof(struct mapple_pslist_fd_stat, attributes));
	stat->attributes_mask = mapple_pslist_fd_read_u64(
		wire, offsetof(struct mapple_pslist_fd_stat, attributes_mask));
	stat->mount_id = mapple_pslist_fd_read_u64(
		wire, offsetof(struct mapple_pslist_fd_stat, mount_id));
	stat->change_cookie = mapple_pslist_fd_read_u64(
		wire, offsetof(struct mapple_pslist_fd_stat, change_cookie));
	stat->filesystem_magic = mapple_pslist_fd_read_u64(
		wire, offsetof(struct mapple_pslist_fd_stat, filesystem_magic));
	mapple_pslist_decode_time(&wire->access_time, &stat->access_time);
	mapple_pslist_decode_time(&wire->birth_time, &stat->birth_time);
	mapple_pslist_decode_time(&wire->change_time, &stat->change_time);
	mapple_pslist_decode_time(&wire->modification_time, &stat->modification_time);
	if (stat->access_time.nanoseconds >= 1000000000U ||
	    stat->birth_time.nanoseconds >= 1000000000U ||
	    stat->change_time.nanoseconds >= 1000000000U ||
	    stat->modification_time.nanoseconds >= 1000000000U)
		return EPROTO;
	return 0;
}

static int mapple_pslist_read_endpoint(int session_fd,
				       const struct mapple_pslist_socket_endpoint *wire,
				       struct mapple_pslist_socket_endpoint_data *endpoint,
				       size_t *remaining_detail)
{
	int ret;

	endpoint->status = mapple_pslist_fd_read_u16(
		wire, offsetof(struct mapple_pslist_socket_endpoint, status));
	endpoint->type = mapple_pslist_fd_read_u16(
		wire, offsetof(struct mapple_pslist_socket_endpoint, type));
	endpoint->error = (__s32)mapple_pslist_fd_read_u32(
		wire, offsetof(struct mapple_pslist_socket_endpoint, error));
	endpoint->port = mapple_pslist_fd_read_u16(
		wire, offsetof(struct mapple_pslist_socket_endpoint, port));
	endpoint->flags = mapple_pslist_fd_read_u16(
		wire, offsetof(struct mapple_pslist_socket_endpoint, flags));
	endpoint->scope_id = mapple_pslist_fd_read_u32(
		wire, offsetof(struct mapple_pslist_socket_endpoint, scope_id));
	endpoint->flow_info = mapple_pslist_fd_read_u32(
		wire, offsetof(struct mapple_pslist_socket_endpoint, flow_info));
	endpoint->netlink_pid = mapple_pslist_fd_read_u32(
		wire, offsetof(struct mapple_pslist_socket_endpoint, netlink_pid));
	endpoint->netlink_groups = mapple_pslist_fd_read_u32(
		wire, offsetof(struct mapple_pslist_socket_endpoint, netlink_groups));
	endpoint->address_length = mapple_pslist_fd_read_u32(
		wire, offsetof(struct mapple_pslist_socket_endpoint, address_length));
	if (endpoint->status < MAPPLE_PSLIST_SOCKET_ENDPOINT_AVAILABLE ||
	    endpoint->status > MAPPLE_PSLIST_SOCKET_ENDPOINT_UNSUPPORTED ||
	    endpoint->type > MAPPLE_PSLIST_SOCKET_ENDPOINT_NETLINK ||
	    (endpoint->flags & ~MAPPLE_PSLIST_SOCKET_ENDPOINT_KNOWN_FLAGS) ||
	    endpoint->address_length > *remaining_detail)
		return EPROTO;
	if (endpoint->status != MAPPLE_PSLIST_SOCKET_ENDPOINT_AVAILABLE &&
	    (endpoint->type != MAPPLE_PSLIST_SOCKET_ENDPOINT_NONE || endpoint->port ||
	     endpoint->flags || endpoint->scope_id || endpoint->flow_info ||
	     endpoint->netlink_pid || endpoint->netlink_groups ||
	     endpoint->address_length))
		return EPROTO;
	if (endpoint->status == MAPPLE_PSLIST_SOCKET_ENDPOINT_AVAILABLE &&
	    endpoint->type == MAPPLE_PSLIST_SOCKET_ENDPOINT_NONE)
		return EPROTO;
	if (endpoint->type == MAPPLE_PSLIST_SOCKET_ENDPOINT_IPV4 &&
	    endpoint->address_length != sizeof(struct in_addr))
		return EPROTO;
	if (endpoint->type == MAPPLE_PSLIST_SOCKET_ENDPOINT_IPV6 &&
	    endpoint->address_length != sizeof(struct in6_addr))
		return EPROTO;
	if ((endpoint->type == MAPPLE_PSLIST_SOCKET_ENDPOINT_NONE ||
	     endpoint->type == MAPPLE_PSLIST_SOCKET_ENDPOINT_NETLINK) &&
	    endpoint->address_length)
		return EPROTO;
	if ((endpoint->flags & MAPPLE_PSLIST_SOCKET_ENDPOINT_ABSTRACT) &&
	    (endpoint->type != MAPPLE_PSLIST_SOCKET_ENDPOINT_UNIX ||
	     !endpoint->address_length))
		return EPROTO;
	if (endpoint->address_length) {
		endpoint->address = malloc(endpoint->address_length);
		if (!endpoint->address)
			return ENOMEM;
		ret = read_exact(session_fd, endpoint->address, endpoint->address_length);
		if (ret)
			return ret;
		if (endpoint->type == MAPPLE_PSLIST_SOCKET_ENDPOINT_UNIX &&
		    (!!(endpoint->flags & MAPPLE_PSLIST_SOCKET_ENDPOINT_ABSTRACT) !=
		     !endpoint->address[0]))
			return EPROTO;
	}
	*remaining_detail -= endpoint->address_length;
	return 0;
}

static int mapple_pslist_read_socket(int session_fd, size_t detail_size,
				     struct mapple_pslist_socket *socket)
{
	struct mapple_pslist_socket_data wire;
	size_t remaining_detail;
	int ret;

	if (detail_size < sizeof(wire))
		return EPROTO;
	ret = read_exact(session_fd, &wire, sizeof(wire));
	if (ret)
		return ret;
	remaining_detail = detail_size - sizeof(wire);
#define DECODE_SOCKET_U16(field) \
	socket->field = mapple_pslist_fd_read_u16(&wire, offsetof(struct mapple_pslist_socket_data, field))
#define DECODE_SOCKET_U32(field) \
	socket->field = mapple_pslist_fd_read_u32(&wire, offsetof(struct mapple_pslist_socket_data, field))
	DECODE_SOCKET_U16(family);
	DECODE_SOCKET_U16(type);
	DECODE_SOCKET_U16(protocol);
	DECODE_SOCKET_U16(socket_state);
	DECODE_SOCKET_U16(protocol_state);
	DECODE_SOCKET_U16(shutdown);
	socket->error = (__s32)mapple_pslist_fd_read_u32(
		&wire, offsetof(struct mapple_pslist_socket_data, error));
	socket->soft_error = (__s32)mapple_pslist_fd_read_u32(
		&wire, offsetof(struct mapple_pslist_socket_data, soft_error));
	DECODE_SOCKET_U32(bound_device);
	DECODE_SOCKET_U32(mark);
	DECODE_SOCKET_U32(priority);
	DECODE_SOCKET_U32(uid);
	DECODE_SOCKET_U32(network_namespace);
	DECODE_SOCKET_U32(receive_buffer);
	DECODE_SOCKET_U32(send_buffer);
	DECODE_SOCKET_U32(receive_queued);
	DECODE_SOCKET_U32(send_queued);
	DECODE_SOCKET_U32(drops);
#undef DECODE_SOCKET_U32
#undef DECODE_SOCKET_U16
	ret = mapple_pslist_read_endpoint(session_fd, &wire.local, &socket->local,
					 &remaining_detail);
	if (!ret)
		ret = mapple_pslist_read_endpoint(session_fd, &wire.peer, &socket->peer,
						 &remaining_detail);
	if (!ret)
		ret = mapple_pslist_fd_skip(session_fd, remaining_detail);
	return ret;
}

static int mapple_pslist_decode_fd(const struct mapple_pslist_fd_record *wire,
				   struct mapple_pslist_fd *fd, __u32 record_size,
				   __u32 *detail_size, __u32 *path_length,
				   __u32 *fdinfo_length)
{
	__u64 minimum_size;

#define DECODE_FD_U16(field) \
	fd->field = mapple_pslist_fd_read_u16(wire, offsetof(struct mapple_pslist_fd_record, field))
#define DECODE_FD_U32(field) \
	fd->field = mapple_pslist_fd_read_u32(wire, offsetof(struct mapple_pslist_fd_record, field))
#define DECODE_FD_U64(field) \
	fd->field = mapple_pslist_fd_read_u64(wire, offsetof(struct mapple_pslist_fd_record, field))
	DECODE_FD_U32(fd);
	DECODE_FD_U16(kind);
	DECODE_FD_U16(detail_type);
	DECODE_FD_U32(descriptor_flags);
	DECODE_FD_U32(open_flags);
	DECODE_FD_U64(file_mode);
	fd->position = (__s64)mapple_pslist_fd_read_u64(
		wire, offsetof(struct mapple_pslist_fd_record, position));
	DECODE_FD_U64(version);
	DECODE_FD_U32(credential_uid);
	DECODE_FD_U32(credential_gid);
	DECODE_FD_U32(credential_effective_uid);
	DECODE_FD_U32(credential_effective_gid);
	DECODE_FD_U32(credential_saved_uid);
	DECODE_FD_U32(credential_saved_gid);
	DECODE_FD_U32(credential_filesystem_uid);
	DECODE_FD_U32(credential_filesystem_gid);
	fd->async_owner_pid = (__s32)mapple_pslist_fd_read_u32(
		wire, offsetof(struct mapple_pslist_fd_record, async_owner_pid));
	DECODE_FD_U16(async_owner_type);
	DECODE_FD_U16(async_signal);
	DECODE_FD_U32(async_owner_uid);
	DECODE_FD_U32(async_owner_effective_uid);
	DECODE_FD_U16(path_status);
	DECODE_FD_U16(stat_status);
	DECODE_FD_U16(fdinfo_status);
	fd->path_error = (__s32)mapple_pslist_fd_read_u32(
		wire, offsetof(struct mapple_pslist_fd_record, path_error));
	fd->stat_error = (__s32)mapple_pslist_fd_read_u32(
		wire, offsetof(struct mapple_pslist_fd_record, stat_error));
	fd->fdinfo_error = (__s32)mapple_pslist_fd_read_u32(
		wire, offsetof(struct mapple_pslist_fd_record, fdinfo_error));
	*detail_size = mapple_pslist_fd_read_u32(
		wire, offsetof(struct mapple_pslist_fd_record, detail_size));
	*path_length = mapple_pslist_fd_read_u32(
		wire, offsetof(struct mapple_pslist_fd_record, path_length));
	*fdinfo_length = mapple_pslist_fd_read_u32(
		wire, offsetof(struct mapple_pslist_fd_record, fdinfo_length));
#undef DECODE_FD_U64
#undef DECODE_FD_U32
#undef DECODE_FD_U16
	minimum_size = sizeof(*wire) + (__u64)*detail_size + *path_length + *fdinfo_length;
	if (wire->reserved || fd->kind < MAPPLE_PSLIST_FD_REGULAR ||
	    fd->kind > MAPPLE_PSLIST_FD_OTHER ||
	    (fd->descriptor_flags & ~MAPPLE_PSLIST_FD_KNOWN_DESCRIPTOR_FLAGS) ||
	    fd->path_status < MAPPLE_PSLIST_DATA_AVAILABLE ||
	    fd->path_status > MAPPLE_PSLIST_DATA_UNSUPPORTED ||
	    fd->stat_status < MAPPLE_PSLIST_DATA_AVAILABLE ||
	    fd->stat_status > MAPPLE_PSLIST_DATA_UNSUPPORTED ||
	    fd->fdinfo_status < MAPPLE_PSLIST_DATA_AVAILABLE ||
	    fd->fdinfo_status > MAPPLE_PSLIST_DATA_UNSUPPORTED ||
	    *fdinfo_length > MAPPLE_PSLIST_FDINFO_MAX_SIZE || minimum_size > record_size ||
	    (fd->path_status == MAPPLE_PSLIST_DATA_AVAILABLE) != !!*path_length ||
	    (fd->fdinfo_status != MAPPLE_PSLIST_DATA_AVAILABLE && *fdinfo_length) ||
	    (fd->path_status == MAPPLE_PSLIST_DATA_AVAILABLE && fd->path_error) ||
	    (fd->stat_status == MAPPLE_PSLIST_DATA_AVAILABLE && fd->stat_error) ||
	    (fd->fdinfo_status == MAPPLE_PSLIST_DATA_AVAILABLE && fd->fdinfo_error) ||
	    (fd->detail_type == MAPPLE_PSLIST_FD_DETAIL_NONE && *detail_size) ||
	    (fd->detail_type == MAPPLE_PSLIST_FD_DETAIL_SOCKET &&
	     fd->kind != MAPPLE_PSLIST_FD_SOCKET))
		return EPROTO;
	return mapple_pslist_decode_stat(&wire->stat, &fd->stat);
}

static int mapple_pslist_read_fd_record(int session_fd, __u32 record_size,
					struct mapple_pslist_fd_table *table)
{
	struct mapple_pslist_fd_record wire;
	struct mapple_pslist_fd fd = { 0 };
	__u32 detail_size;
	__u32 path_length;
	__u32 fdinfo_length;
	__u64 consumed;
	int ret;

	if (record_size < sizeof(wire))
		return EPROTO;
	wire.record_size = htonl(record_size);
	ret = read_exact(session_fd, (unsigned char *)&wire + sizeof(wire.record_size),
			 sizeof(wire) - sizeof(wire.record_size));
	if (ret)
		return ret;
	ret = mapple_pslist_decode_fd(&wire, &fd, record_size, &detail_size, &path_length,
				      &fdinfo_length);
	if (ret)
		return ret;
	if (fd.detail_type == MAPPLE_PSLIST_FD_DETAIL_SOCKET)
		ret = mapple_pslist_read_socket(session_fd, detail_size, &fd.socket);
	else
		ret = mapple_pslist_fd_skip(session_fd, detail_size);
	if (ret)
		goto free_fd;
	if (path_length) {
		fd.path = malloc(path_length);
		if (!fd.path) {
			ret = ENOMEM;
			goto free_fd;
		}
		fd.path_length = path_length;
		ret = read_exact(session_fd, fd.path, path_length);
		if (ret)
			goto free_fd;
	}
	if (fdinfo_length) {
		fd.fdinfo = malloc(fdinfo_length);
		if (!fd.fdinfo) {
			ret = ENOMEM;
			goto free_fd;
		}
		fd.fdinfo_length = fdinfo_length;
		ret = read_exact(session_fd, fd.fdinfo, fdinfo_length);
		if (ret)
			goto free_fd;
	}
	consumed = sizeof(wire) + (__u64)detail_size + path_length + fdinfo_length;
	ret = mapple_pslist_fd_skip(session_fd, record_size - (size_t)consumed);
	if (ret)
		goto free_fd;
	if (table->count && table->entries[table->count - 1].fd >= fd.fd) {
		ret = EPROTO;
		goto free_fd;
	}
	ret = mapple_pslist_fd_grow(table);
	if (ret)
		goto free_fd;
	table->entries[table->count++] = fd;
	return 0;

free_fd:
	mapple_pslist_free_fd(&fd);
	return ret;
}

int mapple_pslist_read_fd_table(int session_fd, struct mapple_pslist_fd_table *table)
{
	struct mapple_pslist_fd_table_end end = { 0 };
	__u32 record_size;
	__u32 record_count;
	int ret;

	for (;;) {
		ret = read_exact(session_fd, &end.record_size, sizeof(end.record_size));
		if (ret)
			return ret;
		record_size = ntohl(end.record_size);
		if (record_size) {
			ret = mapple_pslist_read_fd_record(session_fd, record_size, table);
			if (ret)
				return ret;
			continue;
		}
		ret = read_exact(session_fd, (unsigned char *)&end + sizeof(end.record_size),
				 sizeof(end) - sizeof(end.record_size));
		if (ret)
			return ret;
		table->status = ntohs(end.status);
		record_count = ntohl(end.record_count);
		if (end.reserved || table->status < MAPPLE_PSLIST_FD_TABLE_COMPLETE ||
		    table->status > MAPPLE_PSLIST_FD_TABLE_UNAVAILABLE ||
		    record_count != table->count ||
		    (table->status == MAPPLE_PSLIST_FD_TABLE_UNAVAILABLE && table->count))
			return EPROTO;
		return 0;
	}
}

static const char *mapple_pslist_data_status_name(__u16 status)
{
	switch (status) {
	case MAPPLE_PSLIST_DATA_AVAILABLE:
		return "available";
	case MAPPLE_PSLIST_DATA_UNAVAILABLE:
		return "unavailable";
	case MAPPLE_PSLIST_DATA_TOO_LARGE:
		return "too_large";
	case MAPPLE_PSLIST_DATA_RACED:
		return "raced";
	case MAPPLE_PSLIST_DATA_UNSUPPORTED:
		return "unsupported";
	default:
		return NULL;
	}
}

static const char *mapple_pslist_fd_kind_name(__u16 kind)
{
	static const char *const names[] = {
		[MAPPLE_PSLIST_FD_REGULAR] = "regular",
		[MAPPLE_PSLIST_FD_DIRECTORY] = "directory",
		[MAPPLE_PSLIST_FD_CHARACTER] = "character_device",
		[MAPPLE_PSLIST_FD_BLOCK] = "block_device",
		[MAPPLE_PSLIST_FD_PIPE] = "pipe",
		[MAPPLE_PSLIST_FD_SOCKET] = "socket",
		[MAPPLE_PSLIST_FD_SYMLINK] = "symlink",
		[MAPPLE_PSLIST_FD_ANON_INODE] = "anonymous_inode",
		[MAPPLE_PSLIST_FD_OTHER] = "other",
	};

	return kind < sizeof(names) / sizeof(names[0]) ? names[kind] : NULL;
}

static int mapple_pslist_print_time(const struct mapple_pslist_time *time)
{
	return printf("{\"seconds\": %" PRId64 ", \"nanoseconds\": %u}",
		      (int64_t)time->seconds, time->nanoseconds) < 0 ? EIO : 0;
}

static int mapple_pslist_print_stat(const struct mapple_pslist_fd *fd,
				     int indent)
{
	const struct mapple_pslist_stat *stat = &fd->stat;
	int ret;

	if (fputs("{\n", stdout) == EOF)
		return EIO;
	ret = mapple_pslist_print_json_indent(indent + 2);
	if (ret)
		return ret;
	if (printf("\"status\": \"%s\",\n",
		   mapple_pslist_data_status_name(fd->stat_status)) < 0)
		return EIO;
	ret = mapple_pslist_print_json_indent(indent + 2);
	if (ret)
		return ret;
	if (printf("\"error\": %d", fd->stat_error) < 0)
		return EIO;
	if (fd->stat_status != MAPPLE_PSLIST_DATA_AVAILABLE) {
		if (putchar('\n') == EOF)
			return EIO;
		ret = mapple_pslist_print_json_indent(indent);
		if (ret)
			return ret;
		return putchar('}') == EOF ? EIO : 0;
	}
	if (printf(",\n%*s\"result_mask\": %u,\n%*s\"mode\": %u,"
		   "\n%*s\"link_count\": %u,\n%*s\"block_size\": %u,"
		   "\n%*s\"uid\": %u,\n%*s\"gid\": %u,"
		   "\n%*s\"device_major\": %u,\n%*s\"device_minor\": %u,"
		   "\n%*s\"special_device_major\": %u,"
		   "\n%*s\"special_device_minor\": %u,"
		   "\n%*s\"direct_io_memory_alignment\": %u,"
		   "\n%*s\"direct_io_offset_alignment\": %u,"
		   "\n%*s\"inode\": %" PRIu64 ",\n%*s\"size\": %" PRId64 ","
		   "\n%*s\"blocks\": %" PRIu64 ",\n%*s\"attributes\": %" PRIu64 ","
		   "\n%*s\"attributes_mask\": %" PRIu64 ","
		   "\n%*s\"mount_id\": %" PRIu64 ","
		   "\n%*s\"change_cookie\": %" PRIu64 ","
		   "\n%*s\"filesystem_magic\": %" PRIu64 ",\n%*s\"access_time\": ",
		   indent + 2, "", stat->result_mask, indent + 2, "", stat->mode,
		   indent + 2, "", stat->link_count, indent + 2, "", stat->block_size,
		   indent + 2, "", stat->uid, indent + 2, "", stat->gid,
		   indent + 2, "", stat->device_major, indent + 2, "", stat->device_minor,
		   indent + 2, "", stat->special_device_major, indent + 2, "",
		   stat->special_device_minor, indent + 2, "",
		   stat->direct_io_memory_alignment, indent + 2, "",
		   stat->direct_io_offset_alignment, indent + 2, "", (uint64_t)stat->inode,
		   indent + 2, "", (int64_t)stat->size, indent + 2, "",
		   (uint64_t)stat->blocks, indent + 2, "", (uint64_t)stat->attributes,
		   indent + 2, "", (uint64_t)stat->attributes_mask, indent + 2, "",
		   (uint64_t)stat->mount_id, indent + 2, "", (uint64_t)stat->change_cookie,
		   indent + 2, "", (uint64_t)stat->filesystem_magic, indent + 2, "") < 0)
		return EIO;
	ret = mapple_pslist_print_time(&stat->access_time);
	if (!ret && printf(",\n%*s\"birth_time\": ", indent + 2, "") < 0)
		ret = EIO;
	if (!ret)
		ret = mapple_pslist_print_time(&stat->birth_time);
	if (!ret && printf(",\n%*s\"change_time\": ", indent + 2, "") < 0)
		ret = EIO;
	if (!ret)
		ret = mapple_pslist_print_time(&stat->change_time);
	if (!ret && printf(",\n%*s\"modification_time\": ", indent + 2, "") < 0)
		ret = EIO;
	if (!ret)
		ret = mapple_pslist_print_time(&stat->modification_time);
	if (!ret && putchar('\n') == EOF)
		ret = EIO;
	if (!ret)
		ret = mapple_pslist_print_json_indent(indent);
	if (!ret && putchar('}') == EOF)
		ret = EIO;
	return ret;
}

static const char *mapple_pslist_endpoint_status_name(__u16 status)
{
	switch (status) {
	case MAPPLE_PSLIST_SOCKET_ENDPOINT_AVAILABLE:
		return "available";
	case MAPPLE_PSLIST_SOCKET_ENDPOINT_UNAVAILABLE:
		return "unavailable";
	case MAPPLE_PSLIST_SOCKET_ENDPOINT_NOT_CONNECTED:
		return "not_connected";
	case MAPPLE_PSLIST_SOCKET_ENDPOINT_UNSUPPORTED:
		return "unsupported";
	default:
		return NULL;
	}
}

static const char *mapple_pslist_endpoint_type_name(__u16 type)
{
	switch (type) {
	case MAPPLE_PSLIST_SOCKET_ENDPOINT_NONE:
		return "none";
	case MAPPLE_PSLIST_SOCKET_ENDPOINT_IPV4:
		return "ipv4";
	case MAPPLE_PSLIST_SOCKET_ENDPOINT_IPV6:
		return "ipv6";
	case MAPPLE_PSLIST_SOCKET_ENDPOINT_UNIX:
		return "unix";
	case MAPPLE_PSLIST_SOCKET_ENDPOINT_NETLINK:
		return "netlink";
	default:
		return NULL;
	}
}

static int mapple_pslist_print_endpoint(
	const struct mapple_pslist_socket_endpoint_data *endpoint, int indent)
{
	const char *status = mapple_pslist_endpoint_status_name(endpoint->status);
	const char *type = mapple_pslist_endpoint_type_name(endpoint->type);
	char address[INET6_ADDRSTRLEN];
	const char *text = NULL;
	int ret;

	if (endpoint->type == MAPPLE_PSLIST_SOCKET_ENDPOINT_IPV4 && endpoint->address)
		text = inet_ntop(AF_INET, endpoint->address, address, sizeof(address));
	else if (endpoint->type == MAPPLE_PSLIST_SOCKET_ENDPOINT_IPV6 && endpoint->address)
		text = inet_ntop(AF_INET6, endpoint->address, address, sizeof(address));
	if (!status || !type)
		return EPROTO;
	if (printf("{\n%*s\"status\": \"%s\",\n%*s\"status_value\": %u,"
		   "\n%*s\"error\": %d,\n%*s\"type\": \"%s\","
		   "\n%*s\"type_value\": %u,\n%*s\"port\": %u,"
		   "\n%*s\"flags\": %u,\n%*s\"scope_id\": %u,"
		   "\n%*s\"flow_info\": %u,\n%*s\"netlink_pid\": %u,"
		   "\n%*s\"netlink_groups\": %u,\n%*s\"address\": ",
		   indent + 2, "", status, indent + 2, "", endpoint->status,
		   indent + 2, "", endpoint->error, indent + 2, "", type,
		   indent + 2, "", endpoint->type, indent + 2, "", endpoint->port,
		   indent + 2, "", endpoint->flags, indent + 2, "", endpoint->scope_id,
		   indent + 2, "", endpoint->flow_info, indent + 2, "",
		   endpoint->netlink_pid, indent + 2, "", endpoint->netlink_groups,
		   indent + 2, "") < 0)
		return EIO;
	if (text)
		ret = mapple_pslist_print_json_bytes((const unsigned char *)text, strlen(text));
	else if (endpoint->address)
		ret = mapple_pslist_print_json_bytes(endpoint->address, endpoint->address_length);
	else
		ret = fputs("null", stdout) == EOF ? EIO : 0;
	if (!ret && putchar('\n') == EOF)
		ret = EIO;
	if (!ret)
		ret = mapple_pslist_print_json_indent(indent);
	if (!ret && putchar('}') == EOF)
		ret = EIO;
	return ret;
}

static int mapple_pslist_print_socket(const struct mapple_pslist_socket *socket,
				       int indent)
{
	int ret;

	if (printf("{\n%*s\"family\": %u,\n%*s\"type\": %u,"
		   "\n%*s\"protocol\": %u,\n%*s\"socket_state\": %u,"
		   "\n%*s\"protocol_state\": %u,\n%*s\"shutdown\": %u,"
		   "\n%*s\"error\": %d,\n%*s\"soft_error\": %d,"
		   "\n%*s\"bound_device\": %u,\n%*s\"mark\": %u,"
		   "\n%*s\"priority\": %u,\n%*s\"uid\": %u,"
		   "\n%*s\"network_namespace\": %u,"
		   "\n%*s\"receive_buffer\": %u,\n%*s\"send_buffer\": %u,"
		   "\n%*s\"receive_queued\": %u,\n%*s\"send_queued\": %u,"
		   "\n%*s\"drops\": %u,\n%*s\"local\": ",
		   indent + 2, "", socket->family, indent + 2, "", socket->type,
		   indent + 2, "", socket->protocol, indent + 2, "", socket->socket_state,
		   indent + 2, "", socket->protocol_state, indent + 2, "", socket->shutdown,
		   indent + 2, "", socket->error, indent + 2, "", socket->soft_error,
		   indent + 2, "", socket->bound_device, indent + 2, "", socket->mark,
		   indent + 2, "", socket->priority, indent + 2, "", socket->uid,
		   indent + 2, "", socket->network_namespace, indent + 2, "",
		   socket->receive_buffer, indent + 2, "", socket->send_buffer,
		   indent + 2, "", socket->receive_queued, indent + 2, "",
		   socket->send_queued, indent + 2, "", socket->drops, indent + 2, "") < 0)
		return EIO;
	ret = mapple_pslist_print_endpoint(&socket->local, indent + 2);
	if (!ret && printf(",\n%*s\"peer\": ", indent + 2, "") < 0)
		ret = EIO;
	if (!ret)
		ret = mapple_pslist_print_endpoint(&socket->peer, indent + 2);
	if (!ret && putchar('\n') == EOF)
		ret = EIO;
	if (!ret)
		ret = mapple_pslist_print_json_indent(indent);
	if (!ret && putchar('}') == EOF)
		ret = EIO;
	return ret;
}

enum mapple_pslist_fdinfo_type {
	MAPPLE_PSLIST_FDINFO_UNKNOWN,
	MAPPLE_PSLIST_FDINFO_EPOLL,
	MAPPLE_PSLIST_FDINFO_EVENTFD,
	MAPPLE_PSLIST_FDINFO_TIMERFD,
	MAPPLE_PSLIST_FDINFO_SIGNALFD,
	MAPPLE_PSLIST_FDINFO_INOTIFY,
	MAPPLE_PSLIST_FDINFO_FANOTIFY,
	MAPPLE_PSLIST_FDINFO_PIDFD,
	MAPPLE_PSLIST_FDINFO_USERFAULTFD
};

static int mapple_pslist_path_equals(const struct mapple_pslist_fd *fd,
				      const char *expected)
{
	size_t length = strlen(expected);

	return fd->path && fd->path_length == length &&
	       !memcmp(fd->path, expected, length);
}

static enum mapple_pslist_fdinfo_type
mapple_pslist_fdinfo_type(const struct mapple_pslist_fd *fd)
{
	if (mapple_pslist_path_equals(fd, "anon_inode:[eventpoll]"))
		return MAPPLE_PSLIST_FDINFO_EPOLL;
	if (mapple_pslist_path_equals(fd, "anon_inode:[eventfd]"))
		return MAPPLE_PSLIST_FDINFO_EVENTFD;
	if (mapple_pslist_path_equals(fd, "anon_inode:[timerfd]"))
		return MAPPLE_PSLIST_FDINFO_TIMERFD;
	if (mapple_pslist_path_equals(fd, "anon_inode:[signalfd]"))
		return MAPPLE_PSLIST_FDINFO_SIGNALFD;
	if (mapple_pslist_path_equals(fd, "anon_inode:inotify"))
		return MAPPLE_PSLIST_FDINFO_INOTIFY;
	if (mapple_pslist_path_equals(fd, "anon_inode:[fanotify]"))
		return MAPPLE_PSLIST_FDINFO_FANOTIFY;
	if (mapple_pslist_path_equals(fd, "anon_inode:[pidfd]"))
		return MAPPLE_PSLIST_FDINFO_PIDFD;
	if (mapple_pslist_path_equals(fd, "anon_inode:[userfaultfd]"))
		return MAPPLE_PSLIST_FDINFO_USERFAULTFD;
	return MAPPLE_PSLIST_FDINFO_UNKNOWN;
}

static int mapple_pslist_fdinfo_value(const struct mapple_pslist_fd *fd,
				       const char *key,
				       const unsigned char **value,
				       size_t *value_length)
{
	const unsigned char *line;
	size_t key_length = strlen(key);
	size_t offset = 0;

	while (offset < fd->fdinfo_length) {
		size_t end = offset;

		while (end < fd->fdinfo_length && fd->fdinfo[end] != '\n')
			++end;
		line = fd->fdinfo + offset;
		if (end - offset >= key_length && !memcmp(line, key, key_length)) {
			size_t start = key_length;

			while (start < end - offset &&
			       (line[start] == ' ' || line[start] == '\t'))
				++start;
			*value = line + start;
			*value_length = end - offset - start;
			return 1;
		}
		offset = end < fd->fdinfo_length ? end + 1 : end;
	}
	return 0;
}

static int mapple_pslist_parse_unsigned(const unsigned char *bytes, size_t length,
					 unsigned int base, unsigned long long *value)
{
	unsigned long long result = 0;
	size_t index;

	if (!length || (base != 8 && base != 10 && base != 16))
		return 0;
	for (index = 0; index < length; ++index) {
		unsigned int digit;

		if (bytes[index] >= '0' && bytes[index] <= '9')
			digit = bytes[index] - '0';
		else if (bytes[index] >= 'a' && bytes[index] <= 'f')
			digit = bytes[index] - 'a' + 10;
		else if (bytes[index] >= 'A' && bytes[index] <= 'F')
			digit = bytes[index] - 'A' + 10;
		else
			return 0;
		if (digit >= base || result > (ULLONG_MAX - digit) / base)
			return 0;
		result = result * base + digit;
	}
	*value = result;
	return 1;
}

static int mapple_pslist_print_fdinfo_scalar(const struct mapple_pslist_fd *fd,
					      const char *key, unsigned int base,
					      int *first)
{
	const unsigned char *value;
	unsigned long long decoded;
	size_t length;

	if (!mapple_pslist_fdinfo_value(fd, key, &value, &length) ||
	    !mapple_pslist_parse_unsigned(value, length, base, &decoded))
		return 0;
	if (printf("%s\"%.*s\": %llu", *first ? "" : ", ",
		   (int)(strlen(key) - 1), key, decoded) < 0)
		return EIO;
	*first = 0;
	return 0;
}

static int mapple_pslist_print_fdinfo_text(const struct mapple_pslist_fd *fd,
					    const char *key, int *first)
{
	const unsigned char *value;
	size_t length;
	int ret;

	if (!mapple_pslist_fdinfo_value(fd, key, &value, &length))
		return 0;
	if (printf("%s\"%.*s\": ", *first ? "" : ", ",
		   (int)(strlen(key) - 1), key) < 0)
		return EIO;
	ret = mapple_pslist_print_json_bytes(value, length);
	if (ret)
		return ret;
	*first = 0;
	return 0;
}

static int mapple_pslist_fdinfo_line(char *buffer, size_t buffer_size,
				      const unsigned char *line, size_t length)
{
	if (length >= buffer_size)
		return 0;
	memcpy(buffer, line, length);
	buffer[length] = '\0';
	return 1;
}

static int mapple_pslist_print_fdinfo_time_pair(const struct mapple_pslist_fd *fd,
						 const char *key, int *first)
{
	const unsigned char *value;
	unsigned long long seconds;
	unsigned long long nanoseconds;
	char buffer[128];
	size_t length;
	int consumed;

	if (!mapple_pslist_fdinfo_value(fd, key, &value, &length))
		return 0;
	if (!mapple_pslist_fdinfo_line(buffer, sizeof(buffer), value, length) ||
	    sscanf(buffer, "(%llu, %llu)%n", &seconds, &nanoseconds, &consumed) != 2 ||
	    (size_t)consumed != length)
		return 0;
	if (printf("%s\"%.*s\": {\"seconds\": %llu, \"nanoseconds\": %llu}",
		   *first ? "" : ", ", (int)(strlen(key) - 1), key, seconds,
		   nanoseconds) < 0)
		return EIO;
	*first = 0;
	return 0;
}

static int mapple_pslist_print_fdinfo_userfault_api(const struct mapple_pslist_fd *fd,
						     int *first)
{
	const unsigned char *value;
	unsigned long long api;
	unsigned long long ioctls;
	unsigned int features;
	char buffer[128];
	size_t length;
	int consumed;

	if (!mapple_pslist_fdinfo_value(fd, "API:", &value, &length))
		return 0;
	if (!mapple_pslist_fdinfo_line(buffer, sizeof(buffer), value, length) ||
	    sscanf(buffer, "%llx:%x:%llx%n", &api, &features, &ioctls, &consumed) != 3 ||
	    (size_t)consumed != length)
		return 0;
	if (printf("%s\"API\": {\"api\": %llu, \"features\": %u, "
		   "\"ioctls\": %llu}", *first ? "" : ", ", api, features,
		   ioctls) < 0)
		return EIO;
	*first = 0;
	return 0;
}

static int mapple_pslist_print_fdinfo_fanotify_config(
	const struct mapple_pslist_fd *fd, int *first)
{
	const unsigned char *value;
	unsigned int flags;
	unsigned int event_flags;
	char buffer[128];
	size_t length;
	int consumed;

	if (!mapple_pslist_fdinfo_value(fd, "fanotify flags:", &value, &length))
		return 0;
	if (!mapple_pslist_fdinfo_line(buffer, sizeof(buffer), value, length) ||
	    sscanf(buffer, "%x event-flags:%x%n", &flags, &event_flags, &consumed) != 2 ||
	    (size_t)consumed != length)
		return 0;
	if (printf("%s\"flags\": %u, \"event_flags\": %u",
		   *first ? "" : ", ", flags, event_flags) < 0)
		return EIO;
	*first = 0;
	return 0;
}

static int mapple_pslist_print_fdinfo_unsigned_list(const struct mapple_pslist_fd *fd,
						     const char *key, int *first)
{
	const unsigned char *value;
	unsigned long long decoded;
	size_t token_start;
	size_t length;
	size_t offset;
	int first_value;

	if (!mapple_pslist_fdinfo_value(fd, key, &value, &length))
		return 0;
	offset = 0;
	while (offset < length) {
		while (offset < length && (value[offset] == ' ' || value[offset] == '\t'))
			++offset;
		token_start = offset;
		while (offset < length && value[offset] != ' ' && value[offset] != '\t')
			++offset;
		if (token_start != offset &&
		    !mapple_pslist_parse_unsigned(value + token_start, offset - token_start,
						  10, &decoded))
			return 0;
	}
	if (printf("%s\"%.*s\": [", *first ? "" : ", ",
		   (int)(strlen(key) - 1), key) < 0)
		return EIO;
	offset = 0;
	first_value = 1;
	while (offset < length) {
		while (offset < length && (value[offset] == ' ' || value[offset] == '\t'))
			++offset;
		token_start = offset;
		while (offset < length && value[offset] != ' ' && value[offset] != '\t')
			++offset;
		if (token_start == offset)
			continue;
		mapple_pslist_parse_unsigned(value + token_start, offset - token_start, 10,
						     &decoded);
		if (printf("%s%llu", first_value ? "" : ", ", decoded) < 0)
			return EIO;
		first_value = 0;
	}
	if (putchar(']') == EOF)
		return EIO;
	*first = 0;
	return 0;
}

static int mapple_pslist_print_fdinfo_record(const unsigned char *line, size_t length,
					      enum mapple_pslist_fdinfo_type type,
					      int *recognized)
{
	char buffer[1024];
	int consumed;

	*recognized = 0;
	if (!mapple_pslist_fdinfo_line(buffer, sizeof(buffer), line, length))
		return 0;
	if (type == MAPPLE_PSLIST_FDINFO_EPOLL) {
		unsigned int events;
		unsigned int special_device;
		unsigned long inode;
		unsigned long long data;
		long long position;
		int target_fd;

		if (sscanf(buffer,
			   "tfd: %d events: %x data: %llx pos:%lld ino:%lx sdev:%x%n",
			   &target_fd, &events, &data, &position, &inode,
			   &special_device, &consumed) != 6)
			return 0;
		*recognized = 1;
		return printf("{\"target_fd\": %d, \"events\": %u, \"data\": %llu, "
			      "\"position\": %lld, \"inode\": %lu, "
			      "\"special_device\": %u}", target_fd, events, data, position,
			      inode, special_device) < 0 ? EIO : 0;
	}
	if (type == MAPPLE_PSLIST_FDINFO_INOTIFY) {
		unsigned int watch_descriptor;
		unsigned int special_device;
		unsigned int mask;
		unsigned int ignored_mask;
		unsigned long inode;

		if (sscanf(buffer,
			   "inotify wd:%x ino:%lx sdev:%x mask:%x ignored_mask:%x%n",
			   &watch_descriptor, &inode, &special_device, &mask,
			   &ignored_mask, &consumed) != 5)
			return 0;
		*recognized = 1;
		return printf("{\"watch_descriptor\": %u, \"inode\": %lu, "
			      "\"special_device\": %u, \"mask\": %u, "
			      "\"ignored_mask\": %u}", watch_descriptor, inode,
			      special_device, mask, ignored_mask) < 0 ? EIO : 0;
	}
	if (type == MAPPLE_PSLIST_FDINFO_FANOTIFY) {
		unsigned int identifier;
		unsigned int mark_flags;
		unsigned int mask;
		unsigned int ignored_mask;
		unsigned int special_device;
		unsigned long inode;

		if (sscanf(buffer,
			   "fanotify ino:%lx sdev:%x mflags:%x mask:%x ignored_mask:%x%n",
			   &inode, &special_device, &mark_flags, &mask, &ignored_mask,
			   &consumed) == 5) {
			*recognized = 1;
			return printf("{\"kind\": \"inode\", \"inode\": %lu, "
				      "\"special_device\": %u, \"mark_flags\": %u, "
				      "\"mask\": %u, \"ignored_mask\": %u}", inode,
				      special_device, mark_flags, mask, ignored_mask) < 0 ?
				       EIO : 0;
		}
		if (sscanf(buffer,
			   "fanotify mnt_id:%x mflags:%x mask:%x ignored_mask:%x%n",
			   &identifier, &mark_flags, &mask, &ignored_mask, &consumed) == 4) {
			*recognized = 1;
			return printf("{\"kind\": \"mount\", \"mount_id\": %u, "
				      "\"mark_flags\": %u, \"mask\": %u, "
				      "\"ignored_mask\": %u}", identifier, mark_flags, mask,
				      ignored_mask) < 0 ? EIO : 0;
		}
		if (sscanf(buffer,
			   "fanotify sdev:%x mflags:%x mask:%x ignored_mask:%x%n",
			   &identifier, &mark_flags, &mask, &ignored_mask, &consumed) == 4) {
			*recognized = 1;
			return printf("{\"kind\": \"superblock\", "
				      "\"special_device\": %u, \"mark_flags\": %u, "
				      "\"mask\": %u, \"ignored_mask\": %u}", identifier,
				      mark_flags, mask, ignored_mask) < 0 ? EIO : 0;
		}
	}
	return 0;
}

static int mapple_pslist_print_fdinfo_records(const struct mapple_pslist_fd *fd,
					       enum mapple_pslist_fdinfo_type type)
{
	const char *prefix = type == MAPPLE_PSLIST_FDINFO_EPOLL ? "tfd:" :
			     type == MAPPLE_PSLIST_FDINFO_INOTIFY ? "inotify " :
			     "fanotify ";
	size_t prefix_length = strlen(prefix);
	size_t offset = 0;
	int first = 1;
	int recognized;
	int ret;

	if (fputs("[", stdout) == EOF)
		return EIO;
	while (offset < fd->fdinfo_length) {
		size_t end = offset;

		while (end < fd->fdinfo_length && fd->fdinfo[end] != '\n')
			++end;
		if (type == MAPPLE_PSLIST_FDINFO_FANOTIFY && end - offset >= 15 &&
		    !memcmp(fd->fdinfo + offset, "fanotify flags:", 15)) {
			offset = end < fd->fdinfo_length ? end + 1 : end;
			continue;
		}
		if (end - offset >= prefix_length &&
		    !memcmp(fd->fdinfo + offset, prefix, prefix_length)) {
			if (!first && fputs(", ", stdout) == EOF)
				return EIO;
			ret = mapple_pslist_print_fdinfo_record(
				fd->fdinfo + offset, end - offset, type, &recognized);
			if (ret)
				return ret;
			if (!recognized) {
				ret = mapple_pslist_print_json_bytes(fd->fdinfo + offset,
								 end - offset);
				if (ret)
					return ret;
			}
			first = 0;
		}
		offset = end < fd->fdinfo_length ? end + 1 : end;
	}
	return putchar(']') == EOF ? EIO : 0;
}

static int mapple_pslist_print_fdinfo_parsed(const struct mapple_pslist_fd *fd,
					      enum mapple_pslist_fdinfo_type type)
{
	const char *name;
	int has_records = 0;
	int first = 1;
	int ret;

	switch (type) {
	case MAPPLE_PSLIST_FDINFO_EPOLL:
		name = "epoll";
		has_records = 1;
		break;
	case MAPPLE_PSLIST_FDINFO_EVENTFD:
		name = "eventfd";
		break;
	case MAPPLE_PSLIST_FDINFO_TIMERFD:
		name = "timerfd";
		break;
	case MAPPLE_PSLIST_FDINFO_SIGNALFD:
		name = "signalfd";
		break;
	case MAPPLE_PSLIST_FDINFO_INOTIFY:
		name = "inotify";
		has_records = 1;
		break;
	case MAPPLE_PSLIST_FDINFO_FANOTIFY:
		name = "fanotify";
		has_records = 1;
		break;
	case MAPPLE_PSLIST_FDINFO_PIDFD:
		name = "pidfd";
		break;
	case MAPPLE_PSLIST_FDINFO_USERFAULTFD:
		name = "userfaultfd";
		break;
	default:
		return EPROTO;
	}
	if (printf("{\"type\": \"%s\", \"fields\": {", name) < 0)
		return EIO;
#define PRINT_SCALAR(key, base) do { \
	ret = mapple_pslist_print_fdinfo_scalar(fd, key, base, &first); \
	if (ret) \
		return ret; \
} while (0)
#define PRINT_TEXT(key) do { \
	ret = mapple_pslist_print_fdinfo_text(fd, key, &first); \
	if (ret) \
		return ret; \
} while (0)
	PRINT_SCALAR("pos:", 10);
	PRINT_SCALAR("flags:", 8);
	PRINT_SCALAR("mnt_id:", 10);
	PRINT_SCALAR("ino:", 10);
	switch (type) {
	case MAPPLE_PSLIST_FDINFO_EVENTFD:
		PRINT_SCALAR("eventfd-count:", 16);
		PRINT_SCALAR("eventfd-id:", 10);
		PRINT_SCALAR("eventfd-semaphore:", 10);
		break;
	case MAPPLE_PSLIST_FDINFO_TIMERFD:
		PRINT_SCALAR("clockid:", 10);
		PRINT_SCALAR("ticks:", 10);
		PRINT_SCALAR("settime flags:", 8);
		ret = mapple_pslist_print_fdinfo_time_pair(fd, "it_value:", &first);
		if (ret)
			return ret;
		ret = mapple_pslist_print_fdinfo_time_pair(fd, "it_interval:", &first);
		if (ret)
			return ret;
		break;
	case MAPPLE_PSLIST_FDINFO_SIGNALFD:
		PRINT_TEXT("sigmask:");
		break;
	case MAPPLE_PSLIST_FDINFO_PIDFD:
		PRINT_SCALAR("Pid:", 10);
		ret = mapple_pslist_print_fdinfo_unsigned_list(fd, "NSpid:", &first);
		if (ret)
			return ret;
		break;
	case MAPPLE_PSLIST_FDINFO_USERFAULTFD:
		PRINT_SCALAR("pending:", 10);
		PRINT_SCALAR("total:", 10);
		ret = mapple_pslist_print_fdinfo_userfault_api(fd, &first);
		if (ret)
			return ret;
		break;
	case MAPPLE_PSLIST_FDINFO_FANOTIFY:
		ret = mapple_pslist_print_fdinfo_fanotify_config(fd, &first);
		if (ret)
			return ret;
		break;
	default:
		break;
	}
#undef PRINT_TEXT
#undef PRINT_SCALAR
	if (fputs("}", stdout) == EOF)
		return EIO;
	if (has_records) {
		if (fputs(", \"records\": ", stdout) == EOF)
			return EIO;
		ret = mapple_pslist_print_fdinfo_records(fd, type);
		if (ret)
			return ret;
	}
	return putchar('}') == EOF ? EIO : 0;
}

static int mapple_pslist_print_open_flags(__u32 flags)
{
	struct mapple_pslist_flag_name {
		__u32 value;
		const char *name;
	};
	static const struct mapple_pslist_flag_name names[] = {
		{ O_APPEND, "append" }, { O_ASYNC, "async" }, { O_CLOEXEC, "close_on_exec" },
		{ O_CREAT, "create" }, { O_DIRECT, "direct" }, { O_DIRECTORY, "directory" },
		{ O_DSYNC, "data_sync" }, { O_EXCL, "exclusive" }, { O_LARGEFILE, "large_file" },
		{ O_NOATIME, "no_atime" }, { O_NOCTTY, "no_controlling_tty" },
		{ O_NOFOLLOW, "no_follow" }, { O_NONBLOCK, "nonblocking" },
		{ O_PATH, "path" }, { O_SYNC, "sync" }, { O_TMPFILE, "temporary_file" },
		{ O_TRUNC, "truncate" }
	};
	const char *access;
	size_t index;
	int first = 1;

	switch (flags & O_ACCMODE) {
	case O_RDONLY:
		access = "read_only";
		break;
	case O_WRONLY:
		access = "write_only";
		break;
	case O_RDWR:
		access = "read_write";
		break;
	default:
		access = "unknown";
		break;
	}
	if (printf("{\"value\": %u, \"access\": \"%s\", \"names\": [", flags,
		   access) < 0)
		return EIO;
	for (index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
		if (!names[index].value || (flags & names[index].value) != names[index].value)
			continue;
		if (printf("%s\"%s\"", first ? "" : ", ", names[index].name) < 0)
			return EIO;
		first = 0;
	}
	return fputs("]}", stdout) == EOF ? EIO : 0;
}

static int mapple_pslist_print_fd(const struct mapple_pslist_fd *fd,
				   int indent)
{
	enum mapple_pslist_fdinfo_type fdinfo_type = MAPPLE_PSLIST_FDINFO_UNKNOWN;
	const char *fdinfo_parse_status;
	const char *kind = mapple_pslist_fd_kind_name(fd->kind);
	const char *path_status = mapple_pslist_data_status_name(fd->path_status);
	const char *fdinfo_status = mapple_pslist_data_status_name(fd->fdinfo_status);
	int ret;

	if (!kind || !path_status || !fdinfo_status)
		return EPROTO;
	if (fd->fdinfo_status != MAPPLE_PSLIST_DATA_AVAILABLE)
		fdinfo_parse_status = "not_available";
	else {
		fdinfo_type = mapple_pslist_fdinfo_type(fd);
		fdinfo_parse_status = fdinfo_type == MAPPLE_PSLIST_FDINFO_UNKNOWN ?
				      "unrecognized" : "complete";
	}
	ret = mapple_pslist_print_json_indent(indent);
	if (ret)
		return ret;
	if (printf("{\n%*s\"fd\": %u,\n%*s\"kind\": \"%s\","
		   "\n%*s\"descriptor_flags\": {"
		   "\n%*s\"value\": %u,"
		   "\n%*s\"close_on_exec\": %s"
		   "\n%*s},\n%*s\"open_flags\": ",
		   indent + 2, "", fd->fd, indent + 2, "", kind,
		   indent + 2, "", indent + 4, "", fd->descriptor_flags,
		   indent + 4, "",
		   fd->descriptor_flags & MAPPLE_PSLIST_FD_DESCRIPTOR_CLOEXEC ? "true" : "false",
		   indent + 2, "", indent + 2, "") < 0)
		return EIO;
	ret = mapple_pslist_print_open_flags(fd->open_flags);
	if (ret)
		return ret;
	if (printf(",\n%*s\"file_mode\": %" PRIu64 ","
		   "\n%*s\"position\": %" PRId64 ","
		   "\n%*s\"version\": %" PRIu64 ","
		   "\n%*s\"credentials\": {"
		   "\n%*s\"uid\": %u,\n%*s\"gid\": %u,"
		   "\n%*s\"effective_uid\": %u,\n%*s\"effective_gid\": %u,"
		   "\n%*s\"saved_uid\": %u,\n%*s\"saved_gid\": %u,"
		   "\n%*s\"filesystem_uid\": %u,\n%*s\"filesystem_gid\": %u"
		   "\n%*s},\n%*s\"async_owner\": {"
		   "\n%*s\"pid\": %d,\n%*s\"type\": %u,"
		   "\n%*s\"signal\": %u,\n%*s\"uid\": %u,"
		   "\n%*s\"effective_uid\": %u\n%*s},"
		   "\n%*s\"path_status\": \"%s\","
		   "\n%*s\"path_error\": %d,\n%*s\"path\": ",
		   indent + 2, "", (uint64_t)fd->file_mode,
		   indent + 2, "", (int64_t)fd->position,
		   indent + 2, "", (uint64_t)fd->version,
		   indent + 2, "", indent + 4, "", fd->credential_uid,
		   indent + 4, "", fd->credential_gid, indent + 4, "",
		   fd->credential_effective_uid, indent + 4, "",
		   fd->credential_effective_gid, indent + 4, "", fd->credential_saved_uid,
		   indent + 4, "", fd->credential_saved_gid, indent + 4, "",
		   fd->credential_filesystem_uid, indent + 4, "",
		   fd->credential_filesystem_gid, indent + 2, "", indent + 2, "",
		   indent + 4, "", fd->async_owner_pid, indent + 4, "",
		   fd->async_owner_type, indent + 4, "", fd->async_signal,
		   indent + 4, "", fd->async_owner_uid, indent + 4, "",
		   fd->async_owner_effective_uid, indent + 2, "", indent + 2, "",
		   path_status, indent + 2, "", fd->path_error, indent + 2, "") < 0)
		return EIO;
	if (fd->path)
		ret = mapple_pslist_print_json_bytes(fd->path, fd->path_length);
	else
		ret = fputs("null", stdout) == EOF ? EIO : 0;
	if (!ret && printf(",\n%*s\"stat\": ", indent + 2, "") < 0)
		ret = EIO;
	if (!ret)
		ret = mapple_pslist_print_stat(fd, indent + 2);
	if (!ret && printf(",\n%*s\"socket\": ", indent + 2, "") < 0)
		ret = EIO;
	if (!ret && fd->detail_type == MAPPLE_PSLIST_FD_DETAIL_SOCKET)
		ret = mapple_pslist_print_socket(&fd->socket, indent + 2);
	else if (!ret && fputs("null", stdout) == EOF)
		ret = EIO;
	if (!ret && printf(",\n%*s\"fdinfo\": {"
			   "\n%*s\"status\": \"%s\","
			   "\n%*s\"error\": %d,"
			   "\n%*s\"parse_status\": \"%s\","
			   "\n%*s\"parsed\": ",
			   indent + 2, "", indent + 4, "", fdinfo_status,
			   indent + 4, "", fd->fdinfo_error, indent + 4, "",
			   fdinfo_parse_status, indent + 4, "") < 0)
		ret = EIO;
	if (!ret && fdinfo_type != MAPPLE_PSLIST_FDINFO_UNKNOWN)
		ret = mapple_pslist_print_fdinfo_parsed(fd, fdinfo_type);
	else if (!ret && fputs("null", stdout) == EOF)
		ret = EIO;
	if (!ret && printf(",\n%*s\"raw\": ", indent + 4, "") < 0)
		ret = EIO;
	if (!ret && fd->fdinfo)
		ret = mapple_pslist_print_json_bytes(fd->fdinfo, fd->fdinfo_length);
	else if (!ret && fputs("null", stdout) == EOF)
		ret = EIO;
	if (!ret && printf("\n%*s}\n", indent + 2, "") < 0)
		ret = EIO;
	if (!ret)
		ret = mapple_pslist_print_json_indent(indent);
	if (!ret && putchar('}') == EOF)
		ret = EIO;
	return ret;
}

int mapple_pslist_print_json_fd_table(const struct mapple_pslist_fd_table *table,
				       int indent)
{
	const char *status;
	size_t index;
	int ret;

	switch (table->status) {
	case MAPPLE_PSLIST_FD_TABLE_COMPLETE:
		status = "complete";
		break;
	case MAPPLE_PSLIST_FD_TABLE_PARTIAL:
		status = "partial";
		break;
	case MAPPLE_PSLIST_FD_TABLE_UNAVAILABLE:
		status = "unavailable";
		break;
	default:
		return EPROTO;
	}
	if (printf("{\n%*s\"status\": \"%s\",\n%*s\"fds\": [",
		   indent + 2, "", status, indent + 2, "") < 0)
		return EIO;
	for (index = 0; index < table->count; ++index) {
		if (fputs(index ? ",\n" : "\n", stdout) == EOF)
			return EIO;
		ret = mapple_pslist_print_fd(&table->entries[index], indent + 4);
		if (ret)
			return ret;
	}
	if (table->count && putchar('\n') == EOF)
		return EIO;
	ret = mapple_pslist_print_json_indent(indent + 2);
	if (ret)
		return ret;
	if (fputs("]\n", stdout) == EOF)
		return EIO;
	ret = mapple_pslist_print_json_indent(indent);
	if (ret)
		return ret;
	return putchar('}') == EOF ? EIO : 0;
}
