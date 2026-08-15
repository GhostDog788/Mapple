#include <transport/tcp.h>

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/in.h>
#include <linux/kthread.h>
#include <linux/net.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/socket.h>
#include <linux/sockptr.h>
#include <net/net_namespace.h>
#include <net/sock.h>

#include <shared.h>

#include <control/session.h>

struct mapple_tcp_session {
	struct mapple_session session;
	struct socket *socket;
};

struct mapple_tcp_server {
	struct socket *listen_socket;
	struct task_struct *listener_task;
};

static struct mapple_tcp_server mapple_tcp_server;

static struct mapple_tcp_session *mapple_tcp_from_stream(struct mapple_stream *stream)
{
	struct mapple_session *session = container_of(stream, struct mapple_session, stream);

	return container_of(session, struct mapple_tcp_session, session);
}

static ssize_t mapple_tcp_stream_read(struct mapple_stream *stream, void *buffer, size_t size)
{
	struct mapple_tcp_session *context = mapple_tcp_from_stream(stream);
	struct msghdr message = { };
	struct kvec vector = {
		.iov_base = buffer,
		.iov_len = size,
	};

	return kernel_recvmsg(context->socket, &message, &vector, 1, size, 0);
}

static ssize_t mapple_tcp_stream_write(struct mapple_stream *stream, const void *buffer,
					       size_t size)
{
	struct mapple_tcp_session *context = mapple_tcp_from_stream(stream);
	struct msghdr message = {
		.msg_flags = MSG_NOSIGNAL,
	};
	struct kvec vector = {
		.iov_base = (void *)buffer,
		.iov_len = size,
	};

	return kernel_sendmsg(context->socket, &message, &vector, 1, size);
}

static void mapple_tcp_stream_shutdown(struct mapple_stream *stream)
{
	struct mapple_tcp_session *context = mapple_tcp_from_stream(stream);

	kernel_sock_shutdown(context->socket, SHUT_RDWR);
}

static const struct mapple_stream_ops mapple_tcp_stream_ops = {
	.read_some = mapple_tcp_stream_read,
	.write_some = mapple_tcp_stream_write,
	.shutdown = mapple_tcp_stream_shutdown,
};

static void mapple_tcp_session_release(struct mapple_session *session)
{
	struct mapple_tcp_session *context = container_of(session, struct mapple_tcp_session,
							 session);

	sock_release(context->socket);
	kfree(context);
}

static void mapple_tcp_accept_session(struct socket *socket)
{
	struct mapple_tcp_session *context;
	int ret;

	context = kzalloc(sizeof(*context), GFP_KERNEL);
	if (!context) {
		sock_release(socket);
		return;
	}
	context->socket = socket;
	mapple_session_init(&context->session, MAPPLE_TRANSPORT_TCP, &mapple_tcp_stream_ops,
				    mapple_tcp_session_release);
	ret = mapple_session_register(&context->session);
	if (ret)
		goto put_session;
	ret = mapple_session_start_parser(&context->session);
	if (ret)
		mapple_session_unregister(&context->session);
	else {
		mapple_session_put(&context->session); /* creator reference */
		return;
	}
put_session:
	mapple_session_put(&context->session);
}

static int mapple_tcp_listener(void *)
{
	while (!kthread_should_stop()) {
		struct socket *accepted = NULL;
		int ret = kernel_accept(mapple_tcp_server.listen_socket, &accepted, 0);

		if (ret) {
			if (!kthread_should_stop())
				pr_debug("mapple: TCP accept failed: %d\n", ret);
			continue;
		}
		mapple_tcp_accept_session(accepted);
	}
	return 0;
}

int mapple_tcp_init(void)
{
	struct sockaddr_in address = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = htons(MAPPLE_TCP_PORT),
	};
	int reuse = 1;
	int ret;

	ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP,
			       &mapple_tcp_server.listen_socket);
	if (ret)
		return ret;
	ret = sock_setsockopt(mapple_tcp_server.listen_socket, SOL_SOCKET, SO_REUSEADDR,
			      KERNEL_SOCKPTR(&reuse), sizeof(reuse));
	if (ret)
		goto release_socket;
	ret = kernel_bind(mapple_tcp_server.listen_socket, (struct sockaddr *)&address,
			  sizeof(address));
	if (ret)
		goto release_socket;
	ret = kernel_listen(mapple_tcp_server.listen_socket, MAPPLE_MAX_SESSIONS);
	if (ret)
		goto release_socket;

	mapple_tcp_server.listener_task =
		kthread_run(mapple_tcp_listener, NULL, "mapple-listener");
	if (IS_ERR(mapple_tcp_server.listener_task)) {
		ret = PTR_ERR(mapple_tcp_server.listener_task);
		mapple_tcp_server.listener_task = NULL;
		goto release_socket;
	}
	pr_info("mapple: listening on 0.0.0.0:%u\n", MAPPLE_TCP_PORT);
	return 0;

release_socket:
	sock_release(mapple_tcp_server.listen_socket);
	mapple_tcp_server.listen_socket = NULL;
	return ret;
}

void mapple_tcp_exit(void)
{
	if (!mapple_tcp_server.listen_socket)
		return;
	kernel_sock_shutdown(mapple_tcp_server.listen_socket, SHUT_RDWR);
	if (mapple_tcp_server.listener_task) {
		kthread_stop(mapple_tcp_server.listener_task);
		mapple_tcp_server.listener_task = NULL;
	}
	sock_release(mapple_tcp_server.listen_socket);
	mapple_tcp_server.listen_socket = NULL;
	pr_info("mapple: TCP listener stopped\n");
}
