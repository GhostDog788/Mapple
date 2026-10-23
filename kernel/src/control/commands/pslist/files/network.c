#include <control/commands/pslist/files/network.h>

#include <asm/unaligned.h>
#include <linux/net.h>
#include <linux/user_namespace.h>
#include <net/net_namespace.h>
#include <net/sock.h>
#include <uapi/linux/in.h>
#include <uapi/linux/in6.h>
#include <uapi/linux/netlink.h>

static void mapple_pslist_encode_endpoint(struct mapple_pslist_socket_endpoint *wire,
					  const struct sockaddr_storage *address, int length,
					  u8 *bytes, u32 *bytes_length, bool peer)
{
	if (length < 0) {
		put_unaligned_be16(length == -ENOTCONN && peer ?
			MAPPLE_PSLIST_SOCKET_ENDPOINT_NOT_CONNECTED :
			MAPPLE_PSLIST_SOCKET_ENDPOINT_UNAVAILABLE, &wire->status);
		put_unaligned_be32(-length, &wire->error);
		return;
	}
	put_unaligned_be16(MAPPLE_PSLIST_SOCKET_ENDPOINT_AVAILABLE, &wire->status);
	switch (address->ss_family) {
	case AF_INET: {
		const struct sockaddr_in *inet = (const struct sockaddr_in *)address;

		put_unaligned_be16(MAPPLE_PSLIST_SOCKET_ENDPOINT_IPV4, &wire->type);
		put_unaligned_be16(ntohs(inet->sin_port), &wire->port);
		memcpy(bytes, &inet->sin_addr, sizeof(inet->sin_addr));
		*bytes_length = sizeof(inet->sin_addr);
		break;
	}
	case AF_INET6: {
		const struct sockaddr_in6 *inet6 = (const struct sockaddr_in6 *)address;

		put_unaligned_be16(MAPPLE_PSLIST_SOCKET_ENDPOINT_IPV6, &wire->type);
		put_unaligned_be16(ntohs(inet6->sin6_port), &wire->port);
		put_unaligned_be32(ntohl(inet6->sin6_flowinfo), &wire->flow_info);
		put_unaligned_be32(inet6->sin6_scope_id, &wire->scope_id);
		memcpy(bytes, &inet6->sin6_addr, sizeof(inet6->sin6_addr));
		*bytes_length = sizeof(inet6->sin6_addr);
		break;
	}
	case AF_UNIX: {
		const struct sockaddr_un *unix_address = (const struct sockaddr_un *)address;
		size_t offset = offsetof(struct sockaddr_un, sun_path);
		size_t available = length > offset ? length - offset : 0;

		available = min_t(size_t, available, sizeof(unix_address->sun_path));
		put_unaligned_be16(MAPPLE_PSLIST_SOCKET_ENDPOINT_UNIX, &wire->type);
		if (available && !unix_address->sun_path[0])
			put_unaligned_be16(MAPPLE_PSLIST_SOCKET_ENDPOINT_ABSTRACT, &wire->flags);
		memcpy(bytes, unix_address->sun_path, available);
		*bytes_length = available;
		break;
	}
	case AF_NETLINK: {
		const struct sockaddr_nl *netlink = (const struct sockaddr_nl *)address;

		put_unaligned_be16(MAPPLE_PSLIST_SOCKET_ENDPOINT_NETLINK, &wire->type);
		put_unaligned_be32(netlink->nl_pid, &wire->netlink_pid);
		put_unaligned_be32(netlink->nl_groups, &wire->netlink_groups);
		break;
	}
	default:
		put_unaligned_be16(MAPPLE_PSLIST_SOCKET_ENDPOINT_UNSUPPORTED, &wire->status);
		break;
	}
	put_unaligned_be32(*bytes_length, &wire->address_length);
}

bool mapple_pslist_capture_socket(struct mapple_pslist_captured_socket *captured, struct file *file,
				  u64 object_id, u32 fd)
{
	struct mapple_pslist_socket_data *wire = &captured->wire.socket;
	struct sockaddr_storage address;
	struct socket *socket = sock_from_file(file);
	struct sock *sk;
	int length;

	if (!socket)
		return false;
	put_unaligned_be64(object_id, &captured->wire.object_id);
	put_unaligned_be32(fd, &captured->wire.fd);
	put_unaligned_be16(socket->type, &wire->type);
	put_unaligned_be16(socket->state, &wire->socket_state);
	sk = READ_ONCE(socket->sk);
	if (sk) {
		put_unaligned_be16(READ_ONCE(sk->sk_family), &wire->family);
		put_unaligned_be16(READ_ONCE(sk->sk_protocol), &wire->protocol);
		put_unaligned_be16(READ_ONCE(sk->sk_state), &wire->protocol_state);
		put_unaligned_be16(READ_ONCE(sk->sk_shutdown), &wire->shutdown);
		put_unaligned_be32(READ_ONCE(sk->sk_err), &wire->error);
		put_unaligned_be32(READ_ONCE(sk->sk_err_soft), &wire->soft_error);
		put_unaligned_be32(READ_ONCE(sk->sk_bound_dev_if), &wire->bound_device);
		put_unaligned_be32(READ_ONCE(sk->sk_mark), &wire->mark);
		put_unaligned_be32(READ_ONCE(sk->sk_priority), &wire->priority);
		put_unaligned_be32(from_kuid_munged(&init_user_ns, sock_i_uid(sk)), &wire->uid);
		put_unaligned_be32(sock_net(sk)->ns.inum, &wire->network_namespace);
		put_unaligned_be32(READ_ONCE(sk->sk_rcvbuf), &wire->receive_buffer);
		put_unaligned_be32(READ_ONCE(sk->sk_sndbuf), &wire->send_buffer);
		put_unaligned_be32(sk_rmem_alloc_get(sk), &wire->receive_queued);
		put_unaligned_be32(sk_wmem_alloc_get(sk), &wire->send_queued);
		put_unaligned_be32(atomic_read(&sk->sk_drops), &wire->drops);
	}
	memset(&address, 0, sizeof(address));
	length = kernel_getsockname(socket, (struct sockaddr *)&address);
	mapple_pslist_encode_endpoint(&wire->local, &address, length, captured->local_address,
				      &captured->local_length, false);
	memset(&address, 0, sizeof(address));
	length = kernel_getpeername(socket, (struct sockaddr *)&address);
	mapple_pslist_encode_endpoint(&wire->peer, &address, length, captured->peer_address,
				      &captured->peer_length, true);
	put_unaligned_be32(captured->local_length, &captured->wire.local_address_length);
	put_unaligned_be32(captured->peer_length, &captured->wire.peer_address_length);
	mapple_pslist_encode_record(&captured->wire.record,
				    sizeof(captured->wire) + captured->local_length +
					    captured->peer_length,
				    MAPPLE_PSLIST_STREAM_NETWORK, MAPPLE_PSLIST_GROUP_NETWORK);
	return true;
}

int mapple_pslist_write_socket(struct mapple_pslist_writer *writer,
			       const struct mapple_pslist_captured_socket *captured)
{
	int ret;

	ret = mapple_pslist_write(writer, &captured->wire, sizeof(captured->wire));
	if (!ret)
		ret = mapple_pslist_write(writer, captured->local_address, captured->local_length);
	if (!ret)
		ret = mapple_pslist_write(writer, captured->peer_address, captured->peer_length);
	return ret;
}
