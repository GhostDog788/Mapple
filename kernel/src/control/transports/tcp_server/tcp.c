#include <control/transports/tcp_server/tcp.h>

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/in.h>
#include <linux/kthread.h>
#include <linux/net.h>
#include <linux/printk.h>
#include <linux/socket.h>
#include <linux/sockptr.h>
#include <net/net_namespace.h>
#include <net/sock.h>

#include <control/sessions/session_manager.h>
#include <control/transports/tcp_server/stream_ops.h>
#include <shared.h>

struct mapple_tcp_server {
	struct socket *listen_socket;
	struct task_struct *listener_task;
};

static struct mapple_tcp_server mapple_tcp_server;

static void mapple_tcp_release_backend(void *backend) { sock_release(backend); }

static const struct mapple_transport mapple_tcp_transport = {
	.name = "tcp",
	.stream_ops = &mapple_tcp_stream_ops,
	.release_backend = mapple_tcp_release_backend,
};

static int mapple_tcp_accept_session(struct socket *socket)
{
	/* The manager owns the accepted socket, including on failure. */
	return mapple_session_manager_register(&mapple_tcp_transport, socket);
}

static int mapple_tcp_listener(void *_)
{
	while (!kthread_should_stop()) {
		struct socket *accepted = NULL;
		int ret = kernel_accept(mapple_tcp_server.listen_socket, &accepted, 0);

		if (ret) {
			if (!kthread_should_stop())
				pr_debug("mapple: TCP accept failed: %d\n", ret);
			continue;
		}
		ret = mapple_tcp_accept_session(accepted);
		if (ret && ret != -EBUSY && ret != -ESHUTDOWN)
			pr_debug("mapple: failed to create TCP session: %d\n", ret);
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

	ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &mapple_tcp_server.listen_socket);
	if (ret)
		return ret;
	ret = sock_setsockopt(mapple_tcp_server.listen_socket, SOL_SOCKET, SO_REUSEADDR, KERNEL_SOCKPTR(&reuse),
						  sizeof(reuse));
	if (ret)
		goto release_socket;
	ret = kernel_bind(mapple_tcp_server.listen_socket, (struct sockaddr *)&address, sizeof(address));
	if (ret)
		goto release_socket;
	ret = kernel_listen(mapple_tcp_server.listen_socket, MAPPLE_MAX_SESSIONS);
	if (ret)
		goto release_socket;

	mapple_tcp_server.listener_task = kthread_run(mapple_tcp_listener, NULL, "mapple-listener");
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
