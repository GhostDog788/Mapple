#include <commands/pslist/context.h>

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <commands/pslist/json.h>

static __u16 mapple_pslist_context_read_u16(const void *record, size_t offset)
{
	__u16 value;

	memcpy(&value, (const unsigned char *)record + offset, sizeof(value));
	return ntohs(value);
}

static __u32 mapple_pslist_context_read_u32(const void *record, size_t offset)
{
	__u32 value;

	memcpy(&value, (const unsigned char *)record + offset, sizeof(value));
	return ntohl(value);
}

static __u64 mapple_pslist_context_read_u64(const void *record, size_t offset)
{
	__u64 value;

	memcpy(&value, (const unsigned char *)record + offset, sizeof(value));
	return be64toh(value);
}

static int mapple_pslist_context_data_status_valid(__u16 status)
{
	return status >= MAPPLE_PSLIST_DATA_AVAILABLE &&
	       status <= MAPPLE_PSLIST_DATA_UNSUPPORTED;
}

static int mapple_pslist_context_path_valid(__u16 status, __s32 error,
					     __u32 length)
{
	if (length > MAPPLE_PSLIST_CONTEXT_PATH_MAX_SIZE)
		return 0;
	if (status == MAPPLE_PSLIST_DATA_AVAILABLE)
		return length && !error;
	return !length && error > 0;
}

static int mapple_pslist_context_copy_bytes(unsigned char **destination,
					     const unsigned char **source,
					     __u32 length)
{
	if (!length)
		return 0;
	*destination = malloc(length);
	if (!*destination)
		return ENOMEM;
	memcpy(*destination, *source, length);
	*source += length;
	return 0;
}

int mapple_pslist_decode_context_record(const void *record, size_t size,
					struct mapple_pslist_context *context)
{
	const struct mapple_pslist_context_record *wire = record;
	const unsigned char *bytes;
	__u64 variable_size;
	__u64 object_id;
	__u32 used_namespaces;
	int ret;

	if (size < sizeof(*wire) || context->has_record)
		return EPROTO;
	object_id = mapple_pslist_context_read_u64(
		wire, offsetof(struct mapple_pslist_context_record, object_id));
	if (object_id != context->object_id)
		return EPROTO;
#define CONTEXT_U16(field)                                                        \
	context->field = mapple_pslist_context_read_u16(                            \
		wire, offsetof(struct mapple_pslist_context_record, field))
#define CONTEXT_U32(field)                                                        \
	context->field = mapple_pslist_context_read_u32(                            \
		wire, offsetof(struct mapple_pslist_context_record, field))
#define CONTEXT_U64(field)                                                        \
	context->field = mapple_pslist_context_read_u64(                            \
		wire, offsetof(struct mapple_pslist_context_record, field))
	CONTEXT_U16(cwd_status);
	CONTEXT_U16(root_status);
	CONTEXT_U16(cgroup_status);
	CONTEXT_U16(cgroup_host_path_status);
	CONTEXT_U16(cgroup_namespace_path_status);
	context->cwd_error = (__s32)mapple_pslist_context_read_u32(
		wire, offsetof(struct mapple_pslist_context_record, cwd_error));
	context->root_error = (__s32)mapple_pslist_context_read_u32(
		wire, offsetof(struct mapple_pslist_context_record, root_error));
	context->cgroup_error = (__s32)mapple_pslist_context_read_u32(
		wire, offsetof(struct mapple_pslist_context_record, cgroup_error));
	context->cgroup_host_path_error = (__s32)mapple_pslist_context_read_u32(
		wire, offsetof(struct mapple_pslist_context_record,
			       cgroup_host_path_error));
	context->cgroup_namespace_path_error = (__s32)mapple_pslist_context_read_u32(
		wire, offsetof(struct mapple_pslist_context_record,
			       cgroup_namespace_path_error));
	CONTEXT_U32(cwd_length);
	CONTEXT_U32(root_length);
	CONTEXT_U32(cgroup_host_path_length);
	CONTEXT_U32(cgroup_namespace_path_length);
	CONTEXT_U32(namespace_available_mask);
	CONTEXT_U32(namespace_unsupported_mask);
	CONTEXT_U32(user_namespace);
	CONTEXT_U32(uts_namespace);
	CONTEXT_U32(ipc_namespace);
	CONTEXT_U32(pid_namespace_active);
	CONTEXT_U32(pid_namespace_children);
	CONTEXT_U32(network_namespace);
	CONTEXT_U32(cgroup_namespace);
	CONTEXT_U32(time_namespace);
	CONTEXT_U32(time_namespace_children);
	CONTEXT_U32(cgroup_hierarchy_id);
	CONTEXT_U64(cgroup_id);
	CONTEXT_U64(cgroup_inode);
#undef CONTEXT_U64
#undef CONTEXT_U32
#undef CONTEXT_U16
	variable_size = (__u64)context->cwd_length + context->root_length +
			context->cgroup_host_path_length +
			context->cgroup_namespace_path_length;
	used_namespaces = context->namespace_available_mask |
			  context->namespace_unsupported_mask;
	if (wire->reserved || sizeof(*wire) + variable_size != size ||
	    !mapple_pslist_context_data_status_valid(context->cwd_status) ||
	    !mapple_pslist_context_data_status_valid(context->root_status) ||
	    !mapple_pslist_context_data_status_valid(context->cgroup_status) ||
	    !mapple_pslist_context_data_status_valid(
		    context->cgroup_host_path_status) ||
	    !mapple_pslist_context_data_status_valid(
		    context->cgroup_namespace_path_status) ||
	    !mapple_pslist_context_path_valid(context->cwd_status,
					       context->cwd_error,
					       context->cwd_length) ||
	    !mapple_pslist_context_path_valid(context->root_status,
					       context->root_error,
					       context->root_length) ||
	    !mapple_pslist_context_path_valid(context->cgroup_host_path_status,
					       context->cgroup_host_path_error,
					       context->cgroup_host_path_length) ||
	    !mapple_pslist_context_path_valid(
		    context->cgroup_namespace_path_status,
		    context->cgroup_namespace_path_error,
		    context->cgroup_namespace_path_length) ||
	    (context->namespace_available_mask & context->namespace_unsupported_mask) ||
	    (used_namespaces & ~MAPPLE_PSLIST_NAMESPACE_KNOWN_MASK) ||
	    context->namespace_unsupported_mask != MAPPLE_PSLIST_NAMESPACE_MOUNT)
		return EPROTO;
	if (context->cgroup_status == MAPPLE_PSLIST_DATA_AVAILABLE) {
		if (context->cgroup_error || !context->cgroup_id || !context->cgroup_inode)
			return EPROTO;
	} else if (!context->cgroup_error || context->cgroup_id ||
		   context->cgroup_inode || context->cgroup_hierarchy_id) {
		return EPROTO;
	}
#define CHECK_NAMESPACE(mask, field)                                              \
	if (!!(context->namespace_available_mask & (mask)) != !!context->field)      \
		return EPROTO
	CHECK_NAMESPACE(MAPPLE_PSLIST_NAMESPACE_USER, user_namespace);
	CHECK_NAMESPACE(MAPPLE_PSLIST_NAMESPACE_UTS, uts_namespace);
	CHECK_NAMESPACE(MAPPLE_PSLIST_NAMESPACE_IPC, ipc_namespace);
	CHECK_NAMESPACE(MAPPLE_PSLIST_NAMESPACE_PID_ACTIVE, pid_namespace_active);
	CHECK_NAMESPACE(MAPPLE_PSLIST_NAMESPACE_PID_CHILDREN, pid_namespace_children);
	CHECK_NAMESPACE(MAPPLE_PSLIST_NAMESPACE_NETWORK, network_namespace);
	CHECK_NAMESPACE(MAPPLE_PSLIST_NAMESPACE_CGROUP, cgroup_namespace);
	CHECK_NAMESPACE(MAPPLE_PSLIST_NAMESPACE_TIME, time_namespace);
	CHECK_NAMESPACE(MAPPLE_PSLIST_NAMESPACE_TIME_CHILDREN,
			 time_namespace_children);
#undef CHECK_NAMESPACE
	bytes = (const unsigned char *)record + sizeof(*wire);
	ret = mapple_pslist_context_copy_bytes(&context->cwd, &bytes,
					       context->cwd_length);
	if (!ret)
		ret = mapple_pslist_context_copy_bytes(&context->root, &bytes,
						       context->root_length);
	if (!ret)
		ret = mapple_pslist_context_copy_bytes(
			&context->cgroup_host_path, &bytes,
			context->cgroup_host_path_length);
	if (!ret)
		ret = mapple_pslist_context_copy_bytes(
			&context->cgroup_namespace_path, &bytes,
			context->cgroup_namespace_path_length);
	if (ret)
		return ret;
	context->has_record = 1;
	return 0;
}

void mapple_pslist_free_context(struct mapple_pslist_context *context)
{
	free(context->cwd);
	free(context->root);
	free(context->cgroup_host_path);
	free(context->cgroup_namespace_path);
	memset(context, 0, sizeof(*context));
}

static const char *mapple_pslist_context_status_name(__u16 status)
{
	switch (status) {
	case MAPPLE_PSLIST_DATA_AVAILABLE: return "available";
	case MAPPLE_PSLIST_DATA_UNAVAILABLE: return "unavailable";
	case MAPPLE_PSLIST_DATA_TOO_LARGE: return "too_large";
	case MAPPLE_PSLIST_DATA_RACED: return "raced";
	case MAPPLE_PSLIST_DATA_UNSUPPORTED: return "unsupported";
	default: return NULL;
	}
}

static const char *mapple_pslist_context_object_status_name(__u16 status)
{
	switch (status) {
	case MAPPLE_PSLIST_OBJECT_COMPLETE: return "complete";
	case MAPPLE_PSLIST_OBJECT_PARTIAL: return "partial";
	case MAPPLE_PSLIST_OBJECT_UNAVAILABLE: return "unavailable";
	case MAPPLE_PSLIST_OBJECT_RACED: return "raced";
	case MAPPLE_PSLIST_OBJECT_TOO_LARGE: return "too_large";
	default: return NULL;
	}
}

static int mapple_pslist_print_error(__s32 error)
{
	if (!error)
		return fputs("null", stdout) == EOF ? EIO : 0;
	return printf("{\"code\": %d, \"message\": ", error) < 0 ||
	       mapple_pslist_print_json_bytes((const unsigned char *)strerror(error),
					      strlen(strerror(error))) ||
	       putchar('}') == EOF ? EIO : 0;
}

static int mapple_pslist_print_path(__u16 status, __s32 error,
				     const unsigned char *path, __u32 length)
{
	const char *name = mapple_pslist_context_status_name(status);
	int ret;

	if (!name || printf("{\"collection_status\": \"%s\", \"path\": ", name) < 0)
		return EIO;
	ret = path ? mapple_pslist_print_json_bytes(path, length) :
		     (fputs("null", stdout) == EOF ? EIO : 0);
	if (ret || fputs(", \"error\": ", stdout) == EOF)
		return ret ? ret : EIO;
	ret = mapple_pslist_print_error(error);
	return ret || putchar('}') == EOF ? (ret ? ret : EIO) : 0;
}

static int mapple_pslist_print_namespace(const struct mapple_pslist_context *context,
					  __u32 mask, __u32 inode)
{
	const char *status;

	if (context->namespace_available_mask & mask)
		status = "available";
	else if (context->namespace_unsupported_mask & mask)
		status = "unsupported";
	else
		status = "unavailable";
	if (printf("{\"collection_status\": \"%s\", \"inode\": ", status) < 0)
		return EIO;
	if (inode) {
		if (printf("%u", inode) < 0)
			return EIO;
	} else if (fputs("null", stdout) == EOF) {
		return EIO;
	}
	return putchar('}') == EOF ? EIO : 0;
}

int mapple_pslist_print_json_context(const struct mapple_pslist_context *context,
				      int indent)
{
	const struct {
		const char *name;
		__u32 mask;
		__u32 inode;
	} namespaces[] = {
		{ "user", MAPPLE_PSLIST_NAMESPACE_USER, context->user_namespace },
		{ "uts", MAPPLE_PSLIST_NAMESPACE_UTS, context->uts_namespace },
		{ "ipc", MAPPLE_PSLIST_NAMESPACE_IPC, context->ipc_namespace },
		{ "mount", MAPPLE_PSLIST_NAMESPACE_MOUNT, 0 },
		{ "pid_active", MAPPLE_PSLIST_NAMESPACE_PID_ACTIVE,
		  context->pid_namespace_active },
		{ "pid_children", MAPPLE_PSLIST_NAMESPACE_PID_CHILDREN,
		  context->pid_namespace_children },
		{ "network", MAPPLE_PSLIST_NAMESPACE_NETWORK,
		  context->network_namespace },
		{ "cgroup", MAPPLE_PSLIST_NAMESPACE_CGROUP,
		  context->cgroup_namespace },
		{ "time", MAPPLE_PSLIST_NAMESPACE_TIME, context->time_namespace },
		{ "time_children", MAPPLE_PSLIST_NAMESPACE_TIME_CHILDREN,
		  context->time_namespace_children },
	};
	const char *status = mapple_pslist_context_object_status_name(context->status);
	const char *cgroup_status = mapple_pslist_context_status_name(
		context->cgroup_status);
	size_t index;
	int ret;

	if (!status || !cgroup_status ||
	    printf("{\"collection_status\": \"%s\",\n", status) < 0)
		return EPROTO;
	ret = mapple_pslist_print_json_indent(indent + 2);
	if (ret || fputs("\"filesystem\": {\"cwd\": ", stdout) == EOF)
		return ret ? ret : EIO;
	ret = mapple_pslist_print_path(context->cwd_status, context->cwd_error,
				       context->cwd, context->cwd_length);
	if (ret || fputs(", \"root\": ", stdout) == EOF)
		return ret ? ret : EIO;
	ret = mapple_pslist_print_path(context->root_status, context->root_error,
				       context->root, context->root_length);
	if (ret || fputs("},\n", stdout) == EOF)
		return ret ? ret : EIO;
	ret = mapple_pslist_print_json_indent(indent + 2);
	if (ret || fputs("\"namespaces\": {", stdout) == EOF)
		return ret ? ret : EIO;
	for (index = 0; index < sizeof(namespaces) / sizeof(namespaces[0]); ++index) {
		if (printf("%s\"%s\": ", index ? ", " : "", namespaces[index].name) < 0)
			return EIO;
		ret = mapple_pslist_print_namespace(context, namespaces[index].mask,
						    namespaces[index].inode);
		if (ret)
			return ret;
	}
	if (ret || fputs("},\n", stdout) == EOF)
		return ret ? ret : EIO;
	ret = mapple_pslist_print_json_indent(indent + 2);
	if (ret || printf("\"cgroup\": {\"collection_status\": \"%s\", "
			  "\"scope\": \"default_hierarchy\", \"id\": ",
			  cgroup_status) < 0)
		return ret ? ret : EIO;
	if (context->cgroup_status == MAPPLE_PSLIST_DATA_AVAILABLE) {
		if (printf("%" PRIu64 ", \"inode\": %" PRIu64
			   ", \"hierarchy_id\": %u", (uint64_t)context->cgroup_id,
			   (uint64_t)context->cgroup_inode,
			   context->cgroup_hierarchy_id) < 0)
			return EIO;
	} else if (fputs("null, \"inode\": null, \"hierarchy_id\": null", stdout) == EOF) {
		return EIO;
	}
	if (fputs(", \"error\": ", stdout) == EOF)
		return EIO;
	ret = mapple_pslist_print_error(context->cgroup_error);
	if (ret || fputs(", \"host_path\": ", stdout) == EOF)
		return ret ? ret : EIO;
	ret = mapple_pslist_print_path(context->cgroup_host_path_status,
		context->cgroup_host_path_error, context->cgroup_host_path,
		context->cgroup_host_path_length);
	if (ret || fputs(", \"namespace_path\": ", stdout) == EOF)
		return ret ? ret : EIO;
	ret = mapple_pslist_print_path(context->cgroup_namespace_path_status,
		context->cgroup_namespace_path_error, context->cgroup_namespace_path,
		context->cgroup_namespace_path_length);
	if (ret || fputs("}\n", stdout) == EOF)
		return ret ? ret : EIO;
	ret = mapple_pslist_print_json_indent(indent);
	return ret || putchar('}') == EOF ? (ret ? ret : EIO) : 0;
}
