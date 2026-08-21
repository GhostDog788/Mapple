#include <commands/pslist/pslist.h>

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <stdio.h>

#include <commands.h>
#include <communication/command.h>
#include <communication/stream.h>

int run_pslist(int session_fd)
{
	struct mapple_wire_header response;
	struct mapple_pslist_record record;
	__u64 remaining;
	int ret;

	ret = send_command(session_fd, MAPPLE_COMMAND_PSLIST, NULL, 0);
	if (ret)
		return ret;
	ret = receive_response_header(session_fd, &response);
	if (ret)
		return ret;
	remaining = be64toh(response.payload_size);
	if (ntohs(response.command) != MAPPLE_COMMAND_PSLIST || ntohl(response.flags) ||
		remaining % sizeof(record))
		return EPROTO;

	printf("%5s %5s %1s %s\n", "PID", "PPID", "S", "COMMAND");
	while (remaining) {
		ret = read_exact(session_fd, &record, sizeof(record));
		if (ret)
			return ret;
		printf("%5u %5u %c %.*s\n", ntohl(record.pid), ntohl(record.ppid), record.state,
		       (int)MAPPLE_PSLIST_NAME_SIZE, record.name);
		remaining -= sizeof(record);
	}
	return 0;
}
