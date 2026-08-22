#include <commands/pslist/pslist.h>

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <commands.h>
#include <communication/command.h>
#include <communication/stream.h>

static __u32 mapple_pslist_read_u32(const struct mapple_pslist_record_header *record,
				    size_t offset)
{
	__u32 value;

	memcpy(&value, (const unsigned char *)record + offset, sizeof(value));
	return ntohl(value);
}

static int mapple_pslist_read_command_line(int session_fd, __u32 length)
{
	unsigned char buffer[4096];
	size_t chunk_size;
	size_t index;
	int ret;

	while (length) {
		chunk_size = length < sizeof(buffer) ? length : sizeof(buffer);
		ret = read_exact(session_fd, buffer, chunk_size);
		if (ret)
			return ret;
		for (index = 0; index < chunk_size; ++index) {
			if (!buffer[index])
				buffer[index] = ' ';
		}
		if (fwrite(buffer, 1, chunk_size, stdout) != chunk_size)
			return EIO;
		length -= chunk_size;
	}
	return 0;
}

int run_pslist(int session_fd)
{
	struct mapple_wire_header response;
	const struct mapple_pslist_record_header terminator = { 0 };
	struct mapple_pslist_record_header record;
	__u64 expected_size;
	__u32 command_line_length;
	__u32 record_size;
	int ret;

	ret = send_command(session_fd, MAPPLE_COMMAND_PSLIST, NULL, 0);
	if (ret)
		return ret;
	ret = receive_response_header(session_fd, &response);
	if (ret)
		return ret;
	if (ntohs(response.command) != MAPPLE_COMMAND_PSLIST ||
	    ntohl(response.flags) != MAPPLE_HEADER_FLAG_STREAM || be64toh(response.payload_size))
		return EPROTO;

	printf("%5s %5s %1s %-16s %s\n", "PID", "PPID", "S", "COMM", "COMMAND");
	for (;;) {
		ret = read_exact(session_fd, &record, sizeof(record));
		if (ret)
			return ret;
		record_size = mapple_pslist_read_u32(
			&record, offsetof(struct mapple_pslist_record_header, record_size));
		if (!record_size)
			return memcmp(&record, &terminator, sizeof(record)) ? EPROTO : 0;
		command_line_length = mapple_pslist_read_u32(
			&record, offsetof(struct mapple_pslist_record_header, command_line_length));
		expected_size = sizeof(record) + (__u64)command_line_length;
		if (record.reserved[0] || record.reserved[1] || record.reserved[2] ||
		    !memchr(record.comm, '\0', sizeof(record.comm)) || expected_size != record_size)
			return EPROTO;
		printf("%5u %5u %c %-16s ",
		       mapple_pslist_read_u32(
			       &record, offsetof(struct mapple_pslist_record_header, pid)),
		       mapple_pslist_read_u32(
			       &record, offsetof(struct mapple_pslist_record_header, ppid)),
		       record.state, record.comm);
		ret = mapple_pslist_read_command_line(session_fd, command_line_length);
		if (ret)
			return ret;
		if (putchar('\n') == EOF)
			return EIO;
	}
}
