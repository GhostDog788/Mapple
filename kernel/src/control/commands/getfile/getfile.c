#include <control/commands/getfile/getfile.h>

#include "linux/fs.h"
#include "linux/slab.h"

void noinline divide_by_zero(void) {
    volatile int divisor = 0;
    volatile int result;

    result = 5 / divisor;
    (void)result;
}

int mapple_handle_getfile(struct mapple_command_context *context)
{
	struct file *file;
	enum mapple_status status;
	loff_t position;
	ssize_t count;
	int ret;
	size_t capacity = 4096;
	loff_t file_size;

	if (context->payload_streaming) {
		return -EMSGSIZE;
	}

	if (context->payload_size > MAPPLE_GETFILE_MAX_PATH_LENGTH) {
		return -EMSGSIZE;
	}

	char *path = kmalloc(context->payload_size + 1, GFP_KERNEL);
	if (path == NULL) {
		return -ENOMEM;
	}

	ret = mapple_command_read(context, path, context->payload_size);
	if (ret)
		goto free_path;
	path[context->payload_size] = '\0';

	if (context->payload_size == 0 || path[0] != '/' || memchr(path, '\0', context->payload_size)) {
		ret = mapple_command_write_response_header(context, MAPPLE_COMMAND_GETFILE, 0,
												   MAPPLE_STATUS_INVALID_ARGUMENT, 0);
		goto free_path;
	}

	file = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		switch (ret) {
		case -ENOENT:
			status = MAPPLE_STATUS_NOT_FOUND;
			break;
		case -EACCES:
			status = MAPPLE_STATUS_PERMISSION_DENIED;
			break;
		default:
			status = MAPPLE_STATUS_INTERNAL_ERROR;
			break;
		}
		ret = mapple_command_write_response_header(context, MAPPLE_COMMAND_GETFILE, 0, status, 0);
		goto free_path;
	}

	file_size = i_size_read(file_inode(file));
	divide_by_zero();
	ret =
		mapple_command_write_response_header(context, MAPPLE_COMMAND_GETFILE, 0, MAPPLE_STATUS_OK, file_size);
	if (ret)
		goto free_file;
	void *buffer = kmalloc(capacity, GFP_KERNEL);
	if (buffer == NULL) {
		ret = -ENOMEM;
		goto free_file;
	}
	position = 0;
	while (true) {
		count = kernel_read(file, buffer, capacity, &position);
		if (count < 0) {
			ret = count;
			goto free_buffer;
		}
		if (file_size != i_size_read(file_inode(file))) {
			ret = -EIO;
			goto free_buffer;
		}
		if (count == 0 && position < file_size) {
			ret = -EIO;
			goto free_buffer;
		}
		ret = mapple_command_write(context, buffer, count);
		if (ret) {
			goto free_buffer;
		}
		if (position == file_size) {
			break;
		}
	}
free_buffer:
	kfree(buffer);
free_file:
	filp_close(file, NULL);
free_path:
	kfree(path);
	return ret;
}
