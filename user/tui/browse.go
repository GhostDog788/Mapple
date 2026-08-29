package tui

import (
	"encoding/base64"
	"fmt"
	"image/color"
	"net"
	"sort"
	"strconv"
	"strings"

	"charm.land/lipgloss/v2"

	"github.com/GhostDog788/Mapple/user/api"
)

// browseFrameKind marks the handful of frames that support an operation
// beyond generic cursor navigation -- currently just framePslist, which
// gains "/" search (see handleBrowseKey).
type browseFrameKind int

const (
	frameGeneric browseFrameKind = iota
	framePslist
)

// A browseFrame is one level of the Enter-drills-in/Esc-or-q-backs-out
// browser: a cursor-navigable list of rows. A row either connects to a
// session and pops back out, pushes a new frame (drilling into a
// task/section/fd), or is a leaf (plain data, Enter does nothing).
type browseFrame struct {
	title  string
	rows   []browseRow
	cursor int
	kind   browseFrameKind
}

type browseRow struct {
	label string
	color color.Color // nil = default terminal foreground
	push  func() browseFrame

	// connect carries the session for a `session list` row instead of a
	// closure: a closure here would have to capture a Model method value
	// (e.g. m.connectTo), which binds to the address of whatever Update()
	// call's local copy of m was live at frame-construction time -- not
	// the model bubbletea is actually still rendering by the time Enter
	// is pressed. Plain data avoids that trap entirely; the real, current
	// m in handleBrowseKey applies it directly.
	connect *api.Session

	// viewHistory carries a pslist history entry for a `command pslist
	// history` row, same reasoning as connect above: selecting it needs to
	// set m.lastSnapshot/sort/filter on the model handleBrowseKey is
	// actually holding, which a captured closure can't reach correctly.
	viewHistory *pslistHistoryEntry
}

func (f *browseFrame) clampCursor() {
	if f.cursor < 0 {
		f.cursor = 0
	}
	if f.cursor > len(f.rows)-1 {
		f.cursor = len(f.rows) - 1
	}
	if f.cursor < 0 {
		f.cursor = 0
	}
}

// render renders the frame at the given terminal width; width <= 0 (not yet
// known, e.g. before the first WindowSizeMsg) skips padding rows to it. The
// width is needed so the selected row's reverse-video highlight spans the
// full line instead of stopping at the end of its text, matching the status
// bar's full-width highlight.
func (f browseFrame) render(width int) []string {
	lines := make([]string, 0, len(f.rows)+2)
	lines = append(lines, titleStyle.Render(f.title), "")
	if len(f.rows) == 0 {
		lines = append(lines, "(empty)")
	}
	for i, row := range f.rows {
		prefix := "  "
		style := lipgloss.NewStyle()
		if row.color != nil {
			style = style.Foreground(row.color)
		}
		if i == f.cursor {
			prefix = "▸ "
			style = style.Inherit(cursorStyle)
		}
		if width > 0 {
			// Inline avoids word-wrapping a row that's longer than width --
			// Width would otherwise turn one row into several rendered
			// lines, throwing off the cursor's line-index math below.
			// Padding-only here is fine: the viewport itself hard-cuts
			// (and, since h/l scroll it, lets you page through) anything
			// still too wide once it's actually drawn.
			style = style.Width(width).Inline(true)
		}
		lines = append(lines, style.Render(prefix+row.label))
	}
	return lines
}

// --- session list ---

func sessionListFrame(sessions []api.Session) browseFrame {
	rows := make([]browseRow, len(sessions))
	for i, s := range sessions {
		sess := s
		label := fmt.Sprintf("%-10s [%-6s] %-28s created %s", shortID(sess.Id), sess.State,
			destSummary(sess.Destination), sess.CreatedAt.Format("15:04:05"))
		rows[i] = browseRow{label: label, color: sessionStateColor(sess.State), connect: &sess}
	}
	return browseFrame{title: fmt.Sprintf("sessions (%d) -- enter to connect, esc/q to cancel", len(sessions)), rows: rows}
}

func destSummary(d api.Destination) string {
	switch {
	case d.Type == api.Tcp && d.Host != nil && d.Port != nil:
		return fmt.Sprintf("tcp %s:%d", *d.Host, *d.Port)
	case d.Path != nil:
		return "chardev " + *d.Path
	default:
		return string(d.Type)
	}
}

func shortID(id string) string {
	if len(id) > 8 {
		return id[:8]
	}
	return id
}

// --- pslist: task list -> task detail -> section (identity/security/files) ---

func pslistFrame(snap api.ProcessSnapshot, sortMode int, filterText string) browseFrame {
	var identities, securities, files map[string]interface{}
	if snap.Identities != nil {
		identities = *snap.Identities
	}
	if snap.Securities != nil {
		securities = *snap.Securities
	}
	if snap.Files != nil {
		files = *snap.Files
	}

	tasks := snap.Tasks
	if filterText != "" {
		needle := strings.ToLower(filterText)
		filtered := make([]map[string]interface{}, 0, len(tasks))
		for _, t := range tasks {
			if strings.Contains(strings.ToLower(stringField(t, "Comm")), needle) {
				filtered = append(filtered, t)
			}
		}
		tasks = filtered
	}
	tasks = append([]map[string]interface{}(nil), tasks...)
	sort.SliceStable(tasks, func(i, j int) bool {
		switch sortMode {
		case sortByComm:
			return stringField(tasks[i], "Comm") < stringField(tasks[j], "Comm")
		case sortByState:
			return intField(tasks[i], "State") < intField(tasks[j], "State")
		default:
			return intField(tasks[i], "PID") < intField(tasks[j], "PID")
		}
	})

	rows := make([]browseRow, len(tasks))
	for i, raw := range tasks {
		t := raw
		state := string(rune(intField(t, "State")))
		label := fmt.Sprintf("%-7d %-20s %-4s %s",
			intField(t, "PID"), stringField(t, "Comm"), state, taskTypeName(intField(t, "Type")))
		rows[i] = browseRow{
			label: label,
			color: taskStateColor(state),
			push:  func() browseFrame { return taskDetailFrame(t, identities, securities, files) },
		}
	}
	title := fmt.Sprintf("pslist: %d tasks (sort=%s%s) -- enter to view, esc/q back", len(tasks), pslistSortName(sortMode), filterSuffix(filterText))
	return browseFrame{title: title, rows: rows, kind: framePslist}
}

// pslistHistoryFrame lists past `command pslist` results, most recent
// first; selecting one views it (fresh sort=pid, no filter) without
// re-fetching from the session.
func pslistHistoryFrame(history []*pslistHistoryEntry) browseFrame {
	rows := make([]browseRow, len(history))
	for i := range history {
		entry := history[len(history)-1-i]
		label := fmt.Sprintf("%s -- %d tasks, %d objects, status=%s",
			entry.fetchedAt.Format("15:04:05"), entry.snapshot.TaskCount, entry.snapshot.ObjectCount, entry.snapshot.Status)
		rows[i] = browseRow{label: label, viewHistory: entry}
	}
	title := fmt.Sprintf("pslist history (%d/%d) -- enter to view, esc/q back", len(history), pslistHistoryLimit)
	return browseFrame{title: title, rows: rows}
}

func filterSuffix(filterText string) string {
	if filterText == "" {
		return ""
	}
	return fmt.Sprintf(" filter=%q", filterText)
}

func taskTypeName(t int) string {
	switch t {
	case 2:
		return "process"
	case 3:
		return "kthread"
	default:
		return "thread"
	}
}

func taskDetailFrame(task, identities, securities, files map[string]interface{}) browseFrame {
	pid := intField(task, "PID")
	comm := stringField(task, "Comm")

	rows := []browseRow{
		{label: fmt.Sprintf("pid: %d", pid)},
		{label: fmt.Sprintf("tgid: %d", intField(task, "TGID"))},
		{label: fmt.Sprintf("ppid: %d", intField(task, "PPID"))},
		{label: fmt.Sprintf("comm: %s", comm)},
		{label: fmt.Sprintf("state: %s", string(rune(intField(task, "State"))))},
		{label: fmt.Sprintf("type: %s", taskTypeName(intField(task, "Type")))},
	}

	if id, ok := groupRef(task, "IdentityID"); ok {
		if obj, ok := lookupObject(identities, id); ok {
			rows = append(rows, browseRow{label: "Identity", push: func() browseFrame { return fieldListFrame("identity", obj) }})
		}
	}
	if id, ok := groupRef(task, "SecurityID"); ok {
		if obj, ok := lookupObject(securities, id); ok {
			rows = append(rows, browseRow{label: "Security", push: func() browseFrame { return fieldListFrame("security", obj) }})
		}
	}
	if id, ok := groupRef(task, "FilesID"); ok {
		if obj, ok := lookupObject(files, id); ok {
			rows = append(rows, browseRow{label: "Files", push: func() browseFrame { return fdListFrame(obj) }})
		}
	}

	return browseFrame{title: fmt.Sprintf("task %d (%s) -- enter a section, esc/q back", pid, comm), rows: rows}
}

func groupRef(m map[string]interface{}, key string) (float64, bool) {
	v, ok := m[key]
	if !ok || v == nil {
		return 0, false
	}
	f, ok := v.(float64)
	return f, ok
}

func lookupObject(objects map[string]interface{}, id float64) (map[string]interface{}, bool) {
	if objects == nil {
		return nil, false
	}
	obj, ok := objects[fmt.Sprintf("%d", int64(id))].(map[string]interface{})
	return obj, ok
}

func fdListFrame(filesObj map[string]interface{}) browseFrame {
	fdsRaw, _ := filesObj["FDs"].([]interface{})
	rows := make([]browseRow, 0, len(fdsRaw))
	for _, raw := range fdsRaw {
		entry, ok := raw.(map[string]interface{})
		if !ok {
			continue
		}
		e := entry
		label := fmt.Sprintf("fd %-4d %-10s %s", intField(e, "FD"), fdKindName(intField(e, "Kind")), stringField(e, "Path"))
		rows = append(rows, browseRow{
			label: label,
			push:  func() browseFrame { return fieldListFrame(fmt.Sprintf("fd %d", intField(e, "FD")), e) },
		})
	}
	return browseFrame{title: fmt.Sprintf("files: %d fds -- enter for detail, esc/q back", len(rows)), rows: rows}
}

func fdKindName(k int) string {
	switch k {
	case 1:
		return "regular"
	case 2:
		return "directory"
	case 3:
		return "character"
	case 4:
		return "block"
	case 5:
		return "pipe"
	case 6:
		return "socket"
	case 7:
		return "symlink"
	case 8:
		return "anon_inode"
	default:
		return "other"
	}
}

// fieldListFrame is a leaf: flat key/value rows, nested objects flattened
// with dotted keys (e.g. an fd's "Details.Stat.Mode").
func fieldListFrame(title string, fields map[string]interface{}) browseFrame {
	flat := map[string]interface{}{}
	flattenFields("", fields, flat)
	keys := make([]string, 0, len(flat))
	for k := range flat {
		// ObjectID is the wire protocol's internal object handle, not
		// data about the process -- it's meaningless to a user browsing
		// this section (they already navigated here by task/fd, not by
		// this number) and looks like a stray raw pointer if shown.
		if k == "ObjectID" || strings.HasSuffix(k, ".ObjectID") {
			continue
		}
		keys = append(keys, k)
	}
	sort.Strings(keys)
	rows := make([]browseRow, len(keys))
	for i, k := range keys {
		rows[i] = browseRow{label: fmt.Sprintf("%s: %s", k, displayFieldValue(k, flat[k], flat))}
	}
	return browseFrame{title: title + " -- esc/q back", rows: rows}
}

// displayFieldValue is formatFieldValue plus overrides for the fields the
// kernel side reports as raw wire codes or raw bytes: enum-typed numbers
// (fd kind, socket family/type/state, object/data status, ...) get their
// real name, a socket address gets decoded into an IP or path instead of a
// base64 blob, and an LSM context gets decoded back into its original text.
// siblings is the full flattened field set the row's key came from, needed
// to decode a socket Address using its sibling Type/Flags fields.
func displayFieldValue(key string, v interface{}, siblings map[string]interface{}) string {
	if n, ok := v.(float64); ok {
		if name, ok := namedEnumField(key, n, siblings); ok {
			return name
		}
	}
	if s, ok := v.(string); ok {
		if strings.HasSuffix(key, "Address") {
			return decodeSocketAddress(key, s, siblings)
		}
		if decoded, ok := decodeRawBytesField(key, s); ok {
			return decoded
		}
	}
	return formatFieldValue(v)
}

// namedEnumField translates the small closed-set numeric fields the wire
// protocol (server/internal/moduleclient/wire/pslist.go) and the Linux
// kernel (address family/socket type/protocol/socket state constants)
// report as bare integers. Dispatch is on the key's last dotted segment,
// since that's what names the field regardless of which object it's nested
// under (e.g. "Network.Local.Type" and "Network.Peer.Type" both end in
// "Type"); "Status" is the one name reused for two different enums (an
// object's own completeness vs. a socket endpoint's own status), so it's
// disambiguated by checking whether it hangs off Local/Peer. siblings lets
// Protocol/ProtocolState check the socket's own Family first: IPPROTO_*/
// TCP_* names are only meaningful for an actual INET/INET6 socket -- e.g. a
// netlink socket's numerically-identical Protocol/ProtocolState values mean
// something else entirely (a netlink family number, an unused state), and
// labeling them "IPPROTO_15"/"TCP_CLOSE" would be actively wrong, not just
// unhelpful, so those fields fall back to the raw number for every other
// family instead of guessing.
func namedEnumField(key string, v float64, siblings map[string]interface{}) (string, bool) {
	code := int(v)
	last := key
	if i := strings.LastIndex(key, "."); i >= 0 {
		last = key[i+1:]
	}
	switch last {
	case "Kind":
		return namedCode(fdKindName(code), v), true
	case "Status":
		if strings.HasSuffix(key, "Local.Status") || strings.HasSuffix(key, "Peer.Status") {
			return namedCode(socketEndpointStatusName(code), v), true
		}
		return namedCode(objectStatusName(code), v), true
	case "CommandLineStatus":
		return namedCode(commandLineStatusName(code), v), true
	case "ExecutableStatus":
		return namedCode(executableStatusName(code), v), true
	case "DumpableStatus", "LSMStatus", "LSMProviderStatus", "PathStatus", "IdentityStatus", "StatStatus", "FDInfoStatus":
		return namedCode(dataStatusName(code), v), true
	case "Type":
		return namedCode(socketEndpointTypeName(code), v), true
	case "Family":
		return namedCode(addressFamilyName(code), v), true
	case "SocketType":
		return namedCode(socketTypeName(code), v), true
	case "Protocol":
		if isInetFamily(key, siblings) {
			return namedCode(protocolName(code), v), true
		}
		return "", false
	case "SocketState":
		return namedCode(socketStateName(code), v), true
	case "ProtocolState":
		if !isInetFamily(key, siblings) {
			return "", false
		}
		return namedCode(protocolStateName(code), v), true
	}
	return "", false
}

func namedCode(name string, raw float64) string {
	return fmt.Sprintf("%s (%d)", name, int64(raw))
}

func objectStatusName(c int) string {
	switch c {
	case 1:
		return "complete"
	case 2:
		return "partial"
	case 3:
		return "unavailable"
	case 4:
		return "raced"
	case 5:
		return "too_large"
	default:
		return "unknown"
	}
}

func dataStatusName(c int) string {
	switch c {
	case 1:
		return "available"
	case 2:
		return "unavailable"
	case 3:
		return "too_large"
	case 4:
		return "raced"
	case 5:
		return "unsupported"
	default:
		return "unknown"
	}
}

func commandLineStatusName(c int) string {
	switch c {
	case 1:
		return "available"
	case 2:
		return "empty"
	case 3:
		return "unavailable"
	case 4:
		return "too_large"
	default:
		return "unknown"
	}
}

func executableStatusName(c int) string {
	switch c {
	case 1:
		return "available"
	case 2:
		return "none"
	case 3:
		return "unavailable"
	default:
		return "unknown"
	}
}

func socketEndpointTypeName(t int) string {
	switch t {
	case 0:
		return "none"
	case 1:
		return "ipv4"
	case 2:
		return "ipv6"
	case 3:
		return "unix"
	case 4:
		return "netlink"
	default:
		return "unknown"
	}
}

func socketEndpointStatusName(c int) string {
	switch c {
	case 1:
		return "available"
	case 2:
		return "unavailable"
	case 3:
		return "not_connected"
	case 4:
		return "unsupported"
	default:
		return "unknown"
	}
}

// addressFamilyName, socketTypeName, protocolName, socketStateName and
// protocolStateName are standard Linux/POSIX values (include/linux/socket.h,
// the SOCK_*/IPPROTO_* constants, and net/tcp_states.h), not part of our own
// wire protocol -- the kernel side reports socket->state (socketStateName)
// and sk->sk_state (protocolStateName) directly (see fd.c), and the latter
// reuses the TCP_* enum for non-TCP protocols too (e.g. Unix stream
// sockets), which is standard Linux kernel convention, not a Mapple choice.
func addressFamilyName(f int) string {
	switch f {
	case 0:
		return "AF_UNSPEC"
	case 1:
		return "AF_UNIX"
	case 2:
		return "AF_INET"
	case 10:
		return "AF_INET6"
	case 16:
		return "AF_NETLINK"
	case 17:
		return "AF_PACKET"
	case 38:
		return "AF_ALG"
	case 40:
		return "AF_VSOCK"
	case 42:
		return "AF_QIPCRTR"
	default:
		return fmt.Sprintf("AF_%d", f)
	}
}

func socketTypeName(t int) string {
	switch t {
	case 1:
		return "SOCK_STREAM"
	case 2:
		return "SOCK_DGRAM"
	case 3:
		return "SOCK_RAW"
	case 4:
		return "SOCK_RDM"
	case 5:
		return "SOCK_SEQPACKET"
	case 6:
		return "SOCK_DCCP"
	case 10:
		return "SOCK_PACKET"
	default:
		return fmt.Sprintf("SOCK_%d", t)
	}
}

func protocolName(p int) string {
	switch p {
	case 0:
		return "IPPROTO_IP"
	case 1:
		return "IPPROTO_ICMP"
	case 2:
		return "IPPROTO_IGMP"
	case 6:
		return "IPPROTO_TCP"
	case 17:
		return "IPPROTO_UDP"
	case 41:
		return "IPPROTO_IPV6"
	case 58:
		return "IPPROTO_ICMPV6"
	case 132:
		return "IPPROTO_SCTP"
	default:
		return fmt.Sprintf("IPPROTO_%d", p)
	}
}

func socketStateName(s int) string {
	switch s {
	case 0:
		return "SS_FREE"
	case 1:
		return "SS_UNCONNECTED"
	case 2:
		return "SS_CONNECTING"
	case 3:
		return "SS_CONNECTED"
	case 4:
		return "SS_DISCONNECTING"
	default:
		return fmt.Sprintf("SS_%d", s)
	}
}

func protocolStateName(s int) string {
	switch s {
	case 1:
		return "TCP_ESTABLISHED"
	case 2:
		return "TCP_SYN_SENT"
	case 3:
		return "TCP_SYN_RECV"
	case 4:
		return "TCP_FIN_WAIT1"
	case 5:
		return "TCP_FIN_WAIT2"
	case 6:
		return "TCP_TIME_WAIT"
	case 7:
		return "TCP_CLOSE"
	case 8:
		return "TCP_CLOSE_WAIT"
	case 9:
		return "TCP_LAST_ACK"
	case 10:
		return "TCP_LISTEN"
	case 11:
		return "TCP_CLOSING"
	case 12:
		return "TCP_NEW_SYN_RECV"
	default:
		return fmt.Sprintf("TCP_%d", s)
	}
}

// isInetFamily reports whether key's enclosing object's sibling Family field
// (e.g. "Network.Protocol" -> "Network.Family") is AF_INET or AF_INET6.
func isInetFamily(key string, siblings map[string]interface{}) bool {
	prefix := ""
	if i := strings.LastIndex(key, "."); i >= 0 {
		prefix = key[:i+1]
	}
	fam, ok := siblings[prefix+"Family"].(float64)
	if !ok {
		return false
	}
	f := int(fam)
	return f == 2 || f == 10 // AF_INET, AF_INET6
}

// decodeSocketAddress decodes a SocketEndpoint's base64'd raw address bytes
// using its sibling Type (and, for a Unix socket, Flags' abstract bit) from
// the same flattened field set -- an IPv4/IPv6 address renders as text, a
// Unix path as its (possibly "@"-prefixed abstract) path, anything else as
// hex so nothing is silently hidden.
func decodeSocketAddress(key, encoded string, siblings map[string]interface{}) string {
	raw, err := base64.StdEncoding.DecodeString(encoded)
	if err != nil {
		return encoded
	}
	prefix := strings.TrimSuffix(key, "Address")
	epType := -1
	if v, ok := siblings[prefix+"Type"].(float64); ok {
		epType = int(v)
	}
	switch epType {
	case 1, 2: // ipv4, ipv6
		if len(raw) == 4 || len(raw) == 16 {
			return net.IP(raw).String()
		}
	case 3: // unix
		abstract := false
		if v, ok := siblings[prefix+"Flags"].(float64); ok {
			abstract = int(v)&1 != 0
		}
		if abstract {
			return "@" + strings.TrimSuffix(strings.TrimPrefix(string(raw), "\x00"), "\x00")
		}
		return strings.TrimRight(string(raw), "\x00")
	}
	if len(raw) == 0 {
		return "(none)"
	}
	return fmt.Sprintf("% x", raw)
}

func decodeRawBytesField(key, encoded string) (string, bool) {
	if !strings.HasSuffix(key, "LSMContext") {
		return "", false
	}
	raw, err := base64.StdEncoding.DecodeString(encoded)
	if err != nil || len(raw) == 0 {
		return "", false
	}
	if isPrintable(raw) {
		return string(raw), true
	}
	return fmt.Sprintf("% x", raw), true
}

// formatFieldValue avoids Go's default %v scientific notation for large
// float64s -- every number here round-tripped through JSON from a Go
// integer (nanosecond timestamps in particular), so it should print like
// one even though the free-form decode path only gives us float64. String
// fields get the same treatment for a different reason: CommandLine is
// NUL-separated argv data straight from /proc/PID/cmdline's convention,
// and a NUL byte is invisible in a terminal, so "/sbin/init\x00nokaslr\x00"
// would otherwise render as the unreadable "/sbin/initnokaslr".
func formatFieldValue(v interface{}) string {
	switch val := v.(type) {
	case float64:
		return strconv.FormatFloat(val, 'f', -1, 64)
	case string:
		return strings.TrimSpace(strings.ReplaceAll(val, "\x00", " "))
	case []interface{}:
		parts := make([]string, len(val))
		for i, e := range val {
			parts[i] = formatFieldValue(e)
		}
		return "[" + strings.Join(parts, ", ") + "]"
	case nil:
		return "(none)"
	default:
		return fmt.Sprintf("%v", v)
	}
}

func flattenFields(prefix string, in map[string]interface{}, out map[string]interface{}) {
	for k, v := range in {
		key := k
		if prefix != "" {
			key = prefix + "." + k
		}
		if nested, ok := v.(map[string]interface{}); ok {
			flattenFields(key, nested, out)
		} else {
			out[key] = v
		}
	}
}
