#include <control/transports/tcp_server/stream_ops.h>

#include <linux/net.h>
#include <linux/socket.h>

static ssize_t mapple_tcp_stream_read(struct mapple_stream *stream, void *buffer, size_t size)
{
	struct socket *socket = stream->backend;
	struct msghdr message = {};
	struct kvec vector = {
		.iov_base = buffer,
		.iov_len = size,
	};

	return kernel_recvmsg(socket, &message, &vector, 1, size, 0);
}

static ssize_t mapple_tcp_stream_write(struct mapple_stream *stream, const void *buffer, size_t size)
{
	struct socket *socket = stream->backend;
	struct msghdr message = {
		.msg_flags = MSG_NOSIGNAL,
	};
	struct kvec vector = {
		.iov_base = (void *)buffer,
		.iov_len = size,
	};

	return kernel_sendmsg(socket, &message, &vector, 1, size);
}

static void mapple_tcp_stream_shutdown(struct mapple_stream *stream)
{
	kernel_sock_shutdown(stream->backend, SHUT_RDWR);
}

const struct mapple_stream_ops mapple_tcp_stream_ops = {
	.read_some = mapple_tcp_stream_read,
	.write_some = mapple_tcp_stream_write,
	.shutdown = mapple_tcp_stream_shutdown,
};
