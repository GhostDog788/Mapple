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

// pslistObjects bundles every per-object group map a task can link to, so
// taskDetailFrame and its children take one value instead of a growing list
// of positional map parameters.
type pslistObjects struct {
	identities   map[string]interface{}
	securities   map[string]interface{}
	contexts     map[string]interface{}
	resources    map[string]interface{}
	environments map[string]interface{}
	memoryMaps   map[string]interface{}
	elfs         map[string]interface{}
	files        map[string]interface{}
}

func mapOrNil(m *map[string]interface{}) map[string]interface{} {
	if m == nil {
		return nil
	}
	return *m
}

func pslistFrame(snap api.ProcessSnapshot, sortMode int, filterText string) browseFrame {
	objs := pslistObjects{
		identities:   mapOrNil(snap.Identities),
		securities:   mapOrNil(snap.Securities),
		contexts:     mapOrNil(snap.Contexts),
		resources:    mapOrNil(snap.Resources),
		environments: mapOrNil(snap.Environments),
		memoryMaps:   mapOrNil(snap.MemoryMaps),
		elfs:         mapOrNil(snap.Elfs),
		files:        mapOrNil(snap.Files),
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
			push:  func() browseFrame { return taskDetailFrame(t, objs) },
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

func taskDetailFrame(task map[string]interface{}, objs pslistObjects) browseFrame {
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
		if obj, ok := lookupObject(objs.identities, id); ok {
			rows = append(rows, browseRow{label: "Identity", push: func() browseFrame { return fieldListFrame("identity", obj) }})
		}
	}
	if id, ok := groupRef(task, "SecurityID"); ok {
		if obj, ok := lookupObject(objs.securities, id); ok {
			rows = append(rows, browseRow{label: "Security", push: func() browseFrame { return fieldListFrame("security", obj) }})
		}
	}
	if id, ok := groupRef(task, "ContextID"); ok {
		if obj, ok := lookupObject(objs.contexts, id); ok {
			rows = append(rows, browseRow{label: "Context", push: func() browseFrame { return fieldListFrame("context", obj) }})
		}
	}
	if id, ok := groupRef(task, "ResourcesID"); ok {
		if obj, ok := lookupObject(objs.resources, id); ok {
			rows = append(rows, browseRow{label: "Resources", push: func() browseFrame { return fieldListFrame("resources", obj) }})
		}
	}
	if id, ok := groupRef(task, "EnvironmentID"); ok {
		if obj, ok := lookupObject(objs.environments, id); ok {
			rows = append(rows, browseRow{label: "Environment", push: func() browseFrame { return environmentFrame(obj) }})
		}
	}
	if id, ok := groupRef(task, "MemoryMapsID"); ok {
		if obj, ok := lookupObject(objs.memoryMaps, id); ok {
			rows = append(rows, browseRow{label: "Memory Maps", push: func() browseFrame { return memoryMapsFrame(obj, objs.elfs) }})
		}
	}
	if id, ok := groupRef(task, "FilesID"); ok {
		if obj, ok := lookupObject(objs.files, id); ok {
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

func uint64Field(m map[string]interface{}, key string) uint64 {
	if v, ok := m[key].(float64); ok {
		return uint64(v)
	}
	return 0
}

// environmentFrame shows a task's captured environment as individual
// KEY=VALUE leaf rows, decoded from the raw NUL-delimited bytes the wire
// layer hands back as a base64 JSON string.
func environmentFrame(obj map[string]interface{}) browseFrame {
	rows := []browseRow{
		{label: fmt.Sprintf("AvailabilityStatus: %s", displayFieldValue("AvailabilityStatus", obj["AvailabilityStatus"], obj))},
		{label: fmt.Sprintf("EntryCount: %s", formatFieldValue(obj["EntryCount"]))},
	}
	entryRows := 0
	if raw, ok := obj["Raw"].(string); ok && raw != "" {
		if decoded, err := base64.StdEncoding.DecodeString(raw); err == nil {
			for _, entry := range strings.Split(string(decoded), "\x00") {
				if entry == "" {
					continue
				}
				rows = append(rows, browseRow{label: entry})
				entryRows++
			}
		}
	}
	return browseFrame{title: fmt.Sprintf("environment: %d entries -- esc/q back", entryRows), rows: rows}
}

// memoryMapsFrame lists a task's memory mappings; enter drills into one
// mapping's fields, which itself gains an "ELF" row when the mapping is
// backed by a file mapple has captured ELF metadata for.
func memoryMapsFrame(obj map[string]interface{}, elfs map[string]interface{}) browseFrame {
	mappingsRaw, _ := obj["Mappings"].([]interface{})
	rows := make([]browseRow, 0, len(mappingsRaw))
	for _, raw := range mappingsRaw {
		m, ok := raw.(map[string]interface{})
		if !ok {
			continue
		}
		mapping := m
		start := uint64Field(mapping, "StartAddress")
		end := uint64Field(mapping, "EndAddress")
		name := stringField(mapping, "Path")
		if name == "" {
			name = stringField(mapping, "Name")
		}
		if name == "" {
			name = "[" + mappingNameKindName(intField(mapping, "NameKind")) + "]"
		}
		label := fmt.Sprintf("%012x-%012x %s", start, end, name)
		rows = append(rows, browseRow{
			label: label,
			push:  func() browseFrame { return memoryMapDetailFrame(mapping, elfs) },
		})
	}
	return browseFrame{title: fmt.Sprintf("memory maps: %d mappings -- enter for detail, esc/q back", len(rows)), rows: rows}
}

func memoryMapDetailFrame(mapping, elfs map[string]interface{}) browseFrame {
	rows := flattenedRows(mapping)
	if id, ok := groupRef(mapping, "ELFObjectID"); ok && id != 0 {
		if obj, ok := lookupObject(elfs, id); ok {
			rows = append(rows, browseRow{label: "ELF", push: func() browseFrame { return elfFrame(obj) }})
		}
	}
	start := uint64Field(mapping, "StartAddress")
	end := uint64Field(mapping, "EndAddress")
	return browseFrame{title: fmt.Sprintf("mapping %012x-%012x -- esc/q back", start, end), rows: rows}
}

// elfFrame shows one ELF file's header metadata plus its string and symbol
// tables as two sub-lists -- kept as separate pushes rather than flattened
// inline since a heavily-linked binary can have thousands of symbols.
func elfFrame(obj map[string]interface{}) browseFrame {
	rows := []browseRow{
		{label: fmt.Sprintf("Status: %s", displayFieldValue("Status", obj["Status"], obj))},
	}
	metadata, _ := obj["Metadata"].(map[string]interface{})
	rows = append(rows, flattenedRows(metadata)...)
	stringsRaw, _ := obj["Strings"].([]interface{})
	symbolsRaw, _ := obj["Symbols"].([]interface{})
	rows = append(rows,
		browseRow{label: fmt.Sprintf("Strings (%d)", len(stringsRaw)), push: func() browseFrame { return elfStringListFrame(stringsRaw) }},
		browseRow{label: fmt.Sprintf("Symbols (%d)", len(symbolsRaw)), push: func() browseFrame { return elfSymbolListFrame(symbolsRaw) }},
	)
	return browseFrame{title: "elf -- esc/q back", rows: rows}
}

func elfStringListFrame(items []interface{}) browseFrame {
	rows := make([]browseRow, 0, len(items))
	for _, raw := range items {
		s, ok := raw.(map[string]interface{})
		if !ok {
			continue
		}
		entry := s
		kind := elfStringKindName(intField(entry, "StringKind"))
		bytesRaw, _ := entry["Bytes"].(string)
		label := fmt.Sprintf("%-12s %s", kind, decodeELFStringBytes("Bytes", bytesRaw, entry))
		rows = append(rows, browseRow{
			label: label,
			push: func() browseFrame {
				return fieldListFrame(fmt.Sprintf("elf string %d", intField(entry, "StringIndex")), entry)
			},
		})
	}
	return browseFrame{title: fmt.Sprintf("elf strings: %d -- enter for detail, esc/q back", len(rows)), rows: rows}
}

func elfSymbolListFrame(items []interface{}) browseFrame {
	rows := make([]browseRow, 0, len(items))
	for _, raw := range items {
		s, ok := raw.(map[string]interface{})
		if !ok {
			continue
		}
		entry := s
		kind := elfSymbolKindName(intField(entry, "SymbolKind"))
		label := fmt.Sprintf("%-8s %s", kind, stringField(entry, "Name"))
		rows = append(rows, browseRow{
			label: label,
			push: func() browseFrame {
				return fieldListFrame(fmt.Sprintf("elf symbol %d", intField(entry, "SymbolIndex")), entry)
			},
		})
	}
	return browseFrame{title: fmt.Sprintf("elf symbols: %d -- enter for detail, esc/q back", len(rows)), rows: rows}
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
	rows := flattenedRows(fields)
	return browseFrame{title: title + " -- esc/q back", rows: rows}
}

// flattenedRows renders every field as a leaf row, flattening nested
// objects with dotted keys. ObjectID/ELFObjectID are dropped: they're wire
// protocol handles, not process data, and where ELFObjectID matters
// (memory_map) the caller adds its own dedicated "ELF" row instead of
// showing the raw id.
func flattenedRows(fields map[string]interface{}) []browseRow {
	flat := map[string]interface{}{}
	flattenFields("", fields, flat)
	keys := make([]string, 0, len(flat))
	for k := range flat {
		if k == "ObjectID" || strings.HasSuffix(k, ".ObjectID") || k == "ELFObjectID" {
			continue
		}
		keys = append(keys, k)
	}
	sort.Strings(keys)
	rows := make([]browseRow, len(keys))
	for i, k := range keys {
		rows[i] = browseRow{label: fmt.Sprintf("%s: %s", k, displayFieldValue(k, flat[k], flat))}
	}
	return rows
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
		if isHexAddressField(key) {
			return fmt.Sprintf("%#x", uint64(n))
		}
	}
	if s, ok := v.(string); ok {
		if strings.HasSuffix(key, "Address") {
			return decodeSocketAddress(key, s, siblings)
		}
		if strings.HasSuffix(key, "Bytes") {
			return decodeELFStringBytes(key, s, siblings)
		}
		if decoded, ok := decodeRawBytesField(key, s); ok {
			return decoded
		}
	}
	return formatFieldValue(v)
}

// decodeELFStringBytes decodes an elf_string record's base64'd Bytes using
// its sibling StringKind: a build id is binary (hex), everything else
// (interpreter path, needed/soname, rpath/runpath) is text.
// isHexAddressField picks out the memory/file-offset fields that read like
// addresses (a memory_map's StartAddress/EndAddress, an ELF header's
// EntryAddress/*HeaderOffset), which are conventionally shown in hex
// (matching /proc/pid/maps) rather than as a huge, unreadable decimal.
func isHexAddressField(key string) bool {
	last := key
	if i := strings.LastIndex(key, "."); i >= 0 {
		last = key[i+1:]
	}
	switch last {
	case "StartAddress", "EndAddress", "EntryAddress", "ProgramHeaderOffset", "SectionHeaderOffset", "PageOffset":
		return true
	default:
		return false
	}
}

func decodeELFStringBytes(key, encoded string, siblings map[string]interface{}) string {
	raw, err := base64.StdEncoding.DecodeString(encoded)
	if err != nil {
		return encoded
	}
	prefix := strings.TrimSuffix(key, "Bytes")
	kind, _ := siblings[prefix+"StringKind"].(float64)
	if int(kind) == 6 { // build_id
		return fmt.Sprintf("% x", raw)
	}
	return string(raw)
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
	case "DumpableStatus", "LSMStatus", "LSMProviderStatus", "PathStatus", "IdentityStatus", "StatStatus", "FDInfoStatus",
		"CWDStatus", "RootStatus", "CgroupStatus", "CgroupHostPathStatus", "CgroupNamespacePathStatus",
		"MemoryStatus", "IOStatus", "NetworkStatus", "NameStatus", "FileMetadataStatus":
		return namedCode(dataStatusName(code), v), true
	case "AvailabilityStatus":
		return namedCode(environmentStatusName(code), v), true
	case "MemoryReason":
		return namedCode(memoryReasonName(code), v), true
	case "MappingType":
		return namedCode(mappingTypeName(code), v), true
	case "NameKind":
		return namedCode(mappingNameKindName(code), v), true
	case "StringKind":
		return namedCode(elfStringKindName(code), v), true
	case "SymbolKind":
		return namedCode(elfSymbolKindName(code), v), true
	case "ELFClass":
		return namedCode(elfClassName(code), v), true
	case "DataEncoding":
		return namedCode(elfDataEncodingName(code), v), true
	case "OSABI":
		return namedCode(elfOSABIName(code), v), true
	case "ObjectType":
		return namedCode(elfObjectTypeName(code), v), true
	case "Machine":
		return namedCode(elfMachineName(code), v), true
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
	if names, ok := flagTableFor(key); ok {
		return namedFlags(uint64(v), names), true
	}
	if last == "OpenFlags" {
		return openFlagsName(uint64(v)), true
	}
	return "", false
}

func environmentStatusName(c int) string {
	switch c {
	case 1:
		return "available"
	case 2:
		return "empty"
	case 3:
		return "unavailable"
	case 4:
		return "too_large"
	case 5:
		return "raced"
	default:
		return "unknown"
	}
}

func memoryReasonName(c int) string {
	switch c {
	case 0:
		return "none"
	case 1:
		return "no_mm"
	default:
		return "unknown"
	}
}

func mappingTypeName(c int) string {
	switch c {
	case 1:
		return "anonymous"
	case 2:
		return "file"
	case 3:
		return "special"
	default:
		return "unknown"
	}
}

func mappingNameKindName(c int) string {
	switch c {
	case 0:
		return "none"
	case 1:
		return "heap"
	case 2:
		return "stack"
	case 3:
		return "anonymous"
	case 4:
		return "shared_anonymous"
	case 5:
		return "special"
	default:
		return "unknown"
	}
}

func elfStringKindName(c int) string {
	switch c {
	case 1:
		return "interpreter"
	case 2:
		return "needed"
	case 3:
		return "soname"
	case 4:
		return "rpath"
	case 5:
		return "runpath"
	case 6:
		return "build_id"
	default:
		return "unknown"
	}
}

func elfSymbolKindName(c int) string {
	switch c {
	case 1:
		return "import"
	case 2:
		return "export"
	default:
		return "unknown"
	}
}

// elfClassName, elfDataEncodingName, elfOSABIName, elfObjectTypeName and
// elfMachineName are standard ELF header constants (elf.h), not part of our
// wire protocol -- the kernel reports the raw e_ident/e_type/e_machine
// values directly.
func elfClassName(c int) string {
	switch c {
	case 1:
		return "ELFCLASS32"
	case 2:
		return "ELFCLASS64"
	default:
		return fmt.Sprintf("ELFCLASS_%d", c)
	}
}

func elfDataEncodingName(c int) string {
	switch c {
	case 1:
		return "ELFDATA2LSB"
	case 2:
		return "ELFDATA2MSB"
	default:
		return fmt.Sprintf("ELFDATA_%d", c)
	}
}

func elfOSABIName(c int) string {
	switch c {
	case 0:
		return "ELFOSABI_NONE"
	case 1:
		return "ELFOSABI_HPUX"
	case 2:
		return "ELFOSABI_NETBSD"
	case 3:
		return "ELFOSABI_LINUX"
	case 6:
		return "ELFOSABI_SOLARIS"
	case 9:
		return "ELFOSABI_FREEBSD"
	case 97:
		return "ELFOSABI_ARM"
	case 255:
		return "ELFOSABI_STANDALONE"
	default:
		return fmt.Sprintf("ELFOSABI_%d", c)
	}
}

func elfObjectTypeName(c int) string {
	switch c {
	case 0:
		return "ET_NONE"
	case 1:
		return "ET_REL"
	case 2:
		return "ET_EXEC"
	case 3:
		return "ET_DYN"
	case 4:
		return "ET_CORE"
	default:
		return fmt.Sprintf("ET_%d", c)
	}
}

func elfMachineName(c int) string {
	switch c {
	case 3:
		return "EM_386"
	case 8:
		return "EM_MIPS"
	case 40:
		return "EM_ARM"
	case 62:
		return "EM_X86_64"
	case 183:
		return "EM_AARCH64"
	case 243:
		return "EM_RISCV"
	default:
		return fmt.Sprintf("EM_%d", c)
	}
}

// flagBit is one named bit in a bitmask field.
type flagBit struct {
	mask uint64
	name string
}

// namedFlags renders a bitmask as its set flag names joined with "|",
// followed by the raw hex value in parens, e.g. "READ|WRITE (0x3)" -- never
// just the flags, so a value with bits this table doesn't know about still
// shows its full number instead of silently dropping them. A zero value
// renders as "none (0x0)".
func namedFlags(value uint64, bits []flagBit) string {
	var names []string
	remaining := value
	for _, b := range bits {
		if value&b.mask == b.mask && b.mask != 0 {
			names = append(names, b.name)
			remaining &^= b.mask
		}
	}
	label := "none"
	if len(names) > 0 {
		label = strings.Join(names, "|")
	}
	if remaining != 0 {
		if len(names) > 0 {
			label += fmt.Sprintf("|unknown(%#x)", remaining)
		} else {
			label = fmt.Sprintf("unknown(%#x)", remaining)
		}
	}
	return fmt.Sprintf("%s (%#x)", label, value)
}

// flagTableFor maps a flattened field's last dotted segment to its bitmask
// table, mirroring the MAPPLE_PSLIST_*_FLAG* / VM_* definitions in
// shared/commands.h. OpenFlags is handled separately (openFlagsName) since
// its access-mode bits (O_RDONLY/O_WRONLY/O_RDWR) aren't independent flags.
func flagTableFor(key string) ([]flagBit, bool) {
	last := key
	if i := strings.LastIndex(key, "."); i >= 0 {
		last = key[i+1:]
	}
	switch last {
	case "ExecutableFlags":
		return executableFlagBits, true
	case "DescriptorFlags":
		return fdDescriptorFlagBits, true
	case "NamespaceAvailableMask", "NamespaceUnsupportedMask":
		return namespaceFlagBits, true
	case "VMFlags":
		return vmFlagBits, true
	case "FileFlags":
		return memoryMapFileFlagBits, true
	}
	return nil, false
}

var executableFlagBits = []flagBit{
	{1 << 0, "DELETED"},
	{1 << 1, "PATH_UNAVAILABLE"},
	{1 << 2, "METADATA_UNAVAILABLE"},
}

var fdDescriptorFlagBits = []flagBit{
	{1 << 0, "CLOEXEC"},
}

var namespaceFlagBits = []flagBit{
	{1 << 0, "USER"},
	{1 << 1, "UTS"},
	{1 << 2, "IPC"},
	{1 << 3, "MOUNT"},
	{1 << 4, "PID_ACTIVE"},
	{1 << 5, "PID_CHILDREN"},
	{1 << 6, "NETWORK"},
	{1 << 7, "CGROUP"},
	{1 << 8, "TIME"},
	{1 << 9, "TIME_CHILDREN"},
}

var memoryMapFileFlagBits = []flagBit{
	{1 << 0, "DELETED"},
}

// vmFlagBits mirrors Linux's VM_* mapping flags (shared/commands.h's
// MAPPLE_PSLIST_VM_*), the same set /proc/pid/smaps prints under "VmFlags:".
var vmFlagBits = []flagBit{
	{1 << 0, "READ"},
	{1 << 1, "WRITE"},
	{1 << 2, "EXEC"},
	{1 << 3, "SHARED"},
	{1 << 4, "MAYREAD"},
	{1 << 5, "MAYWRITE"},
	{1 << 6, "MAYEXEC"},
	{1 << 7, "MAYSHARE"},
	{1 << 8, "GROWSDOWN"},
	{1 << 9, "UFFD_MISSING"},
	{1 << 10, "PFNMAP"},
	{1 << 12, "UFFD_WP"},
	{1 << 13, "LOCKED"},
	{1 << 14, "IO"},
	{1 << 15, "SEQ_READ"},
	{1 << 16, "RAND_READ"},
	{1 << 17, "DONTCOPY"},
	{1 << 18, "DONTEXPAND"},
	{1 << 19, "LOCKONFAULT"},
	{1 << 20, "ACCOUNT"},
	{1 << 21, "NORESERVE"},
	{1 << 22, "HUGETLB"},
	{1 << 23, "SYNC"},
	{1 << 25, "WIPEONFORK"},
	{1 << 26, "DONTDUMP"},
	{1 << 27, "SOFTDIRTY"},
	{1 << 28, "MIXEDMAP"},
	{1 << 29, "HUGEPAGE"},
	{1 << 30, "NOHUGEPAGE"},
	{1 << 31, "MERGEABLE"},
}

// openFlagsName decodes an fd's open(2) flags: the low bits are an access
// mode (not independent flags -- O_RDONLY is literally 0, so it can't be a
// bit in a normal flag table), the rest are real bits.
func openFlagsName(value uint64) string {
	mode := "O_RDONLY"
	switch value & 0x3 {
	case 1:
		mode = "O_WRONLY"
	case 2:
		mode = "O_RDWR"
	}
	names := []string{mode}
	remaining := value &^ 0x3
	for _, b := range openFlagBits {
		if remaining&b.mask == b.mask {
			names = append(names, b.name)
			remaining &^= b.mask
		}
	}
	label := strings.Join(names, "|")
	if remaining != 0 {
		label += fmt.Sprintf("|unknown(%#x)", remaining)
	}
	return fmt.Sprintf("%s (%#x)", label, value)
}

var openFlagBits = []flagBit{
	{0o100, "O_CREAT"},
	{0o200, "O_EXCL"},
	{0o400, "O_NOCTTY"},
	{0o1000, "O_TRUNC"},
	{0o2000, "O_APPEND"},
	{0o4000, "O_NONBLOCK"},
	{0o10000, "O_DSYNC"},
	{0o20000, "O_ASYNC"},
	{0o40000, "O_DIRECT"},
	{0o100000, "O_LARGEFILE"},
	{0o200000, "O_DIRECTORY"},
	{0o400000, "O_NOFOLLOW"},
	{0o1000000, "O_NOATIME"},
	{0o2000000, "O_CLOEXEC"},
	{0o10000000, "O_PATH"},
	{0o20000000, "O_TMPFILE"},
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
