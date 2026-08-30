package tui

import (
	"fmt"
	"image/color"
	"sort"
	"strconv"
	"strings"

	"charm.land/lipgloss/v2"

	"github.com/GhostDog788/Mapple/user/api"
)

// browseFrameKind marks the handful of frames that support an operation
// beyond generic cursor navigation -- framePslist gains "/" search and "s"
// sort-cycle, frameGroupChecklist gains space-to-toggle and "f" to fetch
// (see handleBrowseKey).
type browseFrameKind int

const (
	frameGeneric browseFrameKind = iota
	framePslist
	frameGroupChecklist
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

	// recall carries a past REPL command for a `history` row, same
	// reasoning as connect/viewHistory above: selecting it loads the text
	// back into the real input handleBrowseKey is holding (same effect as
	// recalling it with up/down) without running it immediately.
	recall *string
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

// --- pslist group checklist (`command pslist groups`) ---

// pslistGroupNames are the api.PslistGroup enum's own string values, in the
// same order the server declares them in -- also the group checklist's
// display order.
var pslistGroupNames = []string{
	"identity", "environment", "security", "context", "resources",
	"fds", "fds_details", "network", "memory_maps", "elf",
}

// defaultPslistGroupSelection checks every group, matching today's
// behavior (a bare `command pslist` requests everything) until the user
// deliberately opens `command pslist groups` and unchecks something.
func defaultPslistGroupSelection() map[string]bool {
	sel := make(map[string]bool, len(pslistGroupNames))
	for _, g := range pslistGroupNames {
		sel[g] = true
	}
	return sel
}

// groupChecklistFrame lists the 10 pslist groups with a checkbox each;
// handleBrowseKey handles space/enter (toggle a group row) and "f"/enter-
// on-the-Fetch-row (fire the request with only the checked groups).
func groupChecklistFrame(selection map[string]bool) browseFrame {
	rows := make([]browseRow, 0, len(pslistGroupNames)+1)
	for _, g := range pslistGroupNames {
		mark := "[ ]"
		if selection[g] {
			mark = "[x]"
		}
		rows = append(rows, browseRow{label: fmt.Sprintf("%s %s", mark, g)})
	}
	rows = append(rows, browseRow{label: "Fetch", color: sectionRowColor})
	return browseFrame{
		title: "pslist groups (space/enter toggles, f or enter-on-Fetch requests, esc/q cancels)",
		rows:  rows,
		kind:  frameGroupChecklist,
	}
}

// --- pslist: task list -> task detail -> section (identity/security/files) ---
//
// Every value read here already arrived from the server fully decoded (see
// server/pslistview.go): enum fields are strings, bitmasks are name arrays,
// timestamps are ISO-8601, permissions are ls -l-style strings. This file
// only arranges that data into frames/rows -- it never re-parses a raw wire
// code, matching the project's server/frontend responsibility boundary.

// flatTask is one task flattened out of snap.Tasks{processes,kthreads,
// unmatched_threads} for browsing -- kind (process/kthread/thread) is
// implicit in the wire schema (which bucket a task lives in) but still
// worth showing a human scanning the list, so the TUI tracks it itself
// rather than expecting the server to repeat it as a field.
type flatTask struct {
	fields map[string]interface{}
	kind   string
}

func flattenTasks(tasksSection map[string]interface{}) []flatTask {
	var out []flatTask
	processes, _ := tasksSection["processes"].([]interface{})
	for _, raw := range processes {
		p, ok := raw.(map[string]interface{})
		if !ok {
			continue
		}
		if proc, ok := p["process"].(map[string]interface{}); ok {
			out = append(out, flatTask{fields: proc, kind: "process"})
		}
		threads, _ := p["threads"].([]interface{})
		for _, traw := range threads {
			if t, ok := traw.(map[string]interface{}); ok {
				out = append(out, flatTask{fields: t, kind: "thread"})
			}
		}
	}
	kthreads, _ := tasksSection["kthreads"].([]interface{})
	for _, raw := range kthreads {
		if t, ok := raw.(map[string]interface{}); ok {
			out = append(out, flatTask{fields: t, kind: "kthread"})
		}
	}
	unmatched, _ := tasksSection["unmatched_threads"].([]interface{})
	for _, raw := range unmatched {
		if t, ok := raw.(map[string]interface{}); ok {
			out = append(out, flatTask{fields: t, kind: "thread"})
		}
	}
	return out
}

func pslistFrame(snap api.ProcessSnapshot, sortMode int, filterText string) browseFrame {
	objects := snap.Objects
	elfNames := elfSymbolNamesOf(objects)

	tasks := flattenTasks(snap.Tasks)
	if filterText != "" {
		needle := strings.ToLower(filterText)
		filtered := make([]flatTask, 0, len(tasks))
		for _, t := range tasks {
			if strings.Contains(strings.ToLower(stringField(t.fields, "comm")), needle) {
				filtered = append(filtered, t)
			}
		}
		tasks = filtered
	}
	sort.SliceStable(tasks, func(i, j int) bool {
		switch sortMode {
		case sortByComm:
			return stringField(tasks[i].fields, "comm") < stringField(tasks[j].fields, "comm")
		case sortByState:
			return stringField(tasks[i].fields, "state") < stringField(tasks[j].fields, "state")
		default:
			return intField(tasks[i].fields, "pid") < intField(tasks[j].fields, "pid")
		}
	})

	rows := make([]browseRow, len(tasks))
	for i, t := range tasks {
		task := t
		state := stringField(task.fields, "state")
		label := fmt.Sprintf("%-7d %-20s %-13s %s",
			intField(task.fields, "pid"), stringField(task.fields, "comm"), state, task.kind)
		rows[i] = browseRow{
			label: label,
			color: taskStateColor(state),
			push:  func() browseFrame { return taskDetailFrame(task.fields, objects, elfNames) },
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
			entry.fetchedAt.Format("15:04:05"),
			intField(entry.snapshot.Snapshot, "task_count"),
			intField(entry.snapshot.Snapshot, "object_count"),
			stringField(entry.snapshot.Snapshot, "status"))
		rows[i] = browseRow{label: label, viewHistory: entry}
	}
	title := fmt.Sprintf("pslist history (%d/%d) -- enter to view, esc/q back", len(history), pslistHistoryLimit)
	return browseFrame{title: title, rows: rows}
}

// historyFrame lists every submitted REPL command, most recent first --
// modeled on pslistHistoryFrame above, but selecting a row recalls the
// command into the input (see browseRow.recall) rather than viewing data.
func historyFrame(history []string) browseFrame {
	rows := make([]browseRow, len(history))
	for i := range history {
		cmd := history[len(history)-1-i]
		rows[i] = browseRow{label: cmd, recall: &cmd}
	}
	title := fmt.Sprintf("command history (%d) -- enter to recall, esc/q back", len(history))
	return browseFrame{title: title, rows: rows}
}

// clientLogFrame lists retained printError/printSuccess events, most
// recent first -- modeled on pslistHistoryFrame above, but leaf rows only
// (no drill-down; each entry is already a single line).
func clientLogFrame(entries []logEntry) browseFrame {
	rows := make([]browseRow, len(entries))
	for i := range entries {
		e := entries[len(entries)-1-i]
		col := color.Color(lipgloss.Green)
		if e.level == "error" {
			col = lipgloss.Red
		}
		label := fmt.Sprintf("%s  %-7s %s", e.at.Format("15:04:05"), e.level, e.message)
		rows[i] = browseRow{label: label, color: col}
	}
	title := fmt.Sprintf("client log (%d/%d) -- esc/q back", len(entries), clientLogLimit)
	return browseFrame{title: title, rows: rows}
}

func filterSuffix(filterText string) string {
	if filterText == "" {
		return ""
	}
	return fmt.Sprintf(" filter=%q", filterText)
}

func taskDetailFrame(task map[string]interface{}, objects map[string]interface{}, elfNames []string) browseFrame {
	pid := intField(task, "pid")
	comm := stringField(task, "comm")
	groups, _ := task["groups"].(map[string]interface{})

	rows := []browseRow{
		{label: fmt.Sprintf("pid: %d", pid)},
		{label: fmt.Sprintf("tgid: %d", intField(task, "tgid"))},
		{label: fmt.Sprintf("ppid: %d", intField(task, "ppid"))},
		{label: fmt.Sprintf("comm: %s", comm)},
		{label: fmt.Sprintf("state: %s", stringField(task, "state"))},
	}

	addSection := func(group, label string, push func(obj map[string]interface{}) browseFrame) {
		id, ok := groupRef(groups, group)
		if !ok {
			return
		}
		if obj, ok := lookupObjectIn(objects, group, id); ok {
			rows = append(rows, browseRow{label: label, color: sectionRowColor, push: func() browseFrame { return push(obj) }})
		}
	}
	addSection("identity", "Identity", func(obj map[string]interface{}) browseFrame { return fieldListFrame("identity", obj) })
	addSection("security", "Security", func(obj map[string]interface{}) browseFrame { return fieldListFrame("security", obj) })
	addSection("context", "Context", func(obj map[string]interface{}) browseFrame { return fieldListFrame("context", obj) })
	addSection("resources", "Resources", func(obj map[string]interface{}) browseFrame { return fieldListFrame("resources", obj) })
	addSection("environment", "Environment", func(obj map[string]interface{}) browseFrame { return environmentFrame(obj) })
	addSection("memory_maps", "Memory Maps", func(obj map[string]interface{}) browseFrame { return memoryMapsFrame(obj, objects, elfNames) })
	addSection("fds", "Files", func(obj map[string]interface{}) browseFrame { return fdListFrame(obj) })
	addSection("network", "Network", func(obj map[string]interface{}) browseFrame { return networkFrame(obj) })

	return browseFrame{title: fmt.Sprintf("task %d (%s) -- enter a section, esc/q back", pid, comm), rows: rows}
}

func groupRef(groups map[string]interface{}, key string) (float64, bool) {
	v, ok := groups[key]
	if !ok || v == nil {
		return 0, false
	}
	f, ok := v.(float64)
	return f, ok
}

func lookupByID(objects map[string]interface{}, id float64) (map[string]interface{}, bool) {
	if objects == nil {
		return nil, false
	}
	obj, ok := objects[fmt.Sprintf("%d", int64(id))].(map[string]interface{})
	return obj, ok
}

// lookupObjectIn resolves a group name (e.g. "identity") plus an id within
// snap.Objects, which nests every per-group id-map one level deeper than
// the old flat top-level fields did.
func lookupObjectIn(objects map[string]interface{}, group string, id float64) (map[string]interface{}, bool) {
	groupMap, _ := objects[group].(map[string]interface{})
	return lookupByID(groupMap, id)
}

func elfSymbolNamesOf(objects map[string]interface{}) []string {
	elfGroup, _ := objects["elf"].(map[string]interface{})
	raw, _ := elfGroup["symbol_names"].([]interface{})
	names := make([]string, 0, len(raw))
	for _, v := range raw {
		if s, ok := v.(string); ok {
			names = append(names, s)
		}
	}
	return names
}

func elfFilesOf(objects map[string]interface{}) map[string]interface{} {
	elfGroup, _ := objects["elf"].(map[string]interface{})
	files, _ := elfGroup["files"].(map[string]interface{})
	return files
}

func fdListFrame(filesObj map[string]interface{}) browseFrame {
	fdsRaw, _ := filesObj["fds"].([]interface{})
	rows := make([]browseRow, 0, len(fdsRaw))
	for _, raw := range fdsRaw {
		entry, ok := raw.(map[string]interface{})
		if !ok {
			continue
		}
		e := entry
		label := fmt.Sprintf("fd %-4d %-10s %s", intField(e, "fd"), stringField(e, "kind"), stringField(e, "path"))
		rows = append(rows, browseRow{
			label: label,
			color: sectionRowColor,
			push:  func() browseFrame { return fieldListFrame(fmt.Sprintf("fd %d", intField(e, "fd")), e) },
		})
	}
	return browseFrame{title: fmt.Sprintf("files: %d fds -- enter for detail, esc/q back", len(rows)), rows: rows}
}

// networkFrame lists a task's open sockets (a separate objects.network
// group, correlated to fds only by fd number -- matches v8's own split
// rather than embedding network info inside each fd entry).
func networkFrame(obj map[string]interface{}) browseFrame {
	socketsRaw, _ := obj["sockets"].([]interface{})
	rows := make([]browseRow, 0, len(socketsRaw))
	for _, raw := range socketsRaw {
		entry, ok := raw.(map[string]interface{})
		if !ok {
			continue
		}
		e := entry
		net, _ := e["network"].(map[string]interface{})
		label := fmt.Sprintf("fd %-4d family=%s type=%s", intField(e, "fd"), formatFieldValue(net["family"]), formatFieldValue(net["type"]))
		rows = append(rows, browseRow{
			label: label,
			color: sectionRowColor,
			push:  func() browseFrame { return fieldListFrame(fmt.Sprintf("network fd %d", intField(e, "fd")), e) },
		})
	}
	return browseFrame{title: fmt.Sprintf("network: %d sockets -- enter for detail, esc/q back", len(rows)), rows: rows}
}

// environmentFrame shows a task's captured environment as individual
// KEY=VALUE leaf rows from the server's name/value entry array (see
// viewEnvironment).
func environmentFrame(obj map[string]interface{}) browseFrame {
	rows := []browseRow{
		{label: fmt.Sprintf("collection_status: %s", stringField(obj, "collection_status"))},
	}
	block, _ := obj["block"].(map[string]interface{})
	entries, _ := block["entries"].([]interface{})
	for _, raw := range entries {
		e, ok := raw.(map[string]interface{})
		if !ok {
			continue
		}
		rows = append(rows, browseRow{label: fmt.Sprintf("%s=%s", stringField(e, "name"), stringField(e, "value"))})
	}
	return browseFrame{title: fmt.Sprintf("environment: %d entries -- esc/q back", len(entries)), rows: rows}
}

// mappedFileName resolves a memory_map's "file" reference (a
// objects.mapped_files id) to its path, for the mapping-list label -- falls
// back to the mapping's own type in brackets (e.g. "[anonymous]") when
// there's no backing file or no resolved path.
func mappedFileName(mapping map[string]interface{}, mappedFiles map[string]interface{}) string {
	if fileID, ok := mapping["file"].(float64); ok {
		if fileObj, ok := lookupByID(mappedFiles, fileID); ok {
			if path, ok := fileObj["path"].(map[string]interface{}); ok {
				if v, ok := path["value"].(string); ok && v != "" {
					return v
				}
			}
		}
	}
	return "[" + stringField(mapping, "type") + "]"
}

// memoryMapsFrame lists a task's memory mappings; enter drills into one
// mapping's fields, which itself gains a "File"/"ELF" row when the mapping
// is backed by a file mapple has captured identity/ELF metadata for.
func memoryMapsFrame(obj map[string]interface{}, objects map[string]interface{}, symbolNames []string) browseFrame {
	mappingsRaw, _ := obj["mappings"].([]interface{})
	mappedFiles, _ := objects["mapped_files"].(map[string]interface{})
	rows := make([]browseRow, 0, len(mappingsRaw))
	for _, raw := range mappingsRaw {
		m, ok := raw.(map[string]interface{})
		if !ok {
			continue
		}
		mapping := m
		label := fmt.Sprintf("%s-%s %s %s", stringField(mapping, "start"), stringField(mapping, "end"),
			stringField(mapping, "permissions"), mappedFileName(mapping, mappedFiles))
		rows = append(rows, browseRow{
			label: label,
			color: sectionRowColor,
			push:  func() browseFrame { return memoryMapDetailFrame(mapping, objects, symbolNames) },
		})
	}
	return browseFrame{title: fmt.Sprintf("memory maps: %d mappings -- enter for detail, esc/q back", len(rows)), rows: rows}
}

func memoryMapDetailFrame(mapping map[string]interface{}, objects map[string]interface{}, symbolNames []string) browseFrame {
	start := stringField(mapping, "start")
	end := stringField(mapping, "end")
	title := fmt.Sprintf("mapping %s-%s", start, end)
	rows := objectRows(title, mapping)
	if fileID, ok := mapping["file"].(float64); ok {
		mappedFiles, _ := objects["mapped_files"].(map[string]interface{})
		if fileObj, ok := lookupByID(mappedFiles, fileID); ok {
			rows = append(rows, browseRow{label: "File", color: sectionRowColor, push: func() browseFrame { return fieldListFrame("mapped file", fileObj) }})
			if elfID, ok := fileObj["elf"].(float64); ok {
				if elfObj, ok := lookupByID(elfFilesOf(objects), elfID); ok {
					rows = append(rows, browseRow{label: "ELF", color: sectionRowColor, push: func() browseFrame { return elfFrame(elfObj, symbolNames) }})
				}
			}
		}
	}
	return browseFrame{title: title + " -- esc/q back", rows: rows}
}

// elfFrame shows one ELF file's header/identity/interpreter-needed-etc.
// fields plus its import and export tables as separate pushes, since a
// heavily-linked binary can have thousands of symbols. Import/export symbol
// names are interned snapshot-wide (see server/pslistview.go's
// elfSymbolCatalog) -- symbolNames is that shared table, resolved here by
// index rather than the TUI re-parsing anything.
func elfFrame(obj map[string]interface{}, symbolNames []string) browseFrame {
	imports, _ := obj["imports"].(map[string]interface{})
	importGroups, _ := imports["groups"].([]interface{})
	exports, _ := obj["exports"].(map[string]interface{})
	exportGroups, _ := exports["groups"].([]interface{})

	fields := make(map[string]interface{}, len(obj))
	for k, v := range obj {
		if k == "imports" || k == "exports" {
			continue
		}
		fields[k] = v
	}
	rows := objectRows("elf", fields)
	rows = append(rows,
		browseRow{label: fmt.Sprintf("Imports (%d)", countGroupedSymbols(importGroups)), color: sectionRowColor, push: func() browseFrame { return elfSymbolGroupsFrame("imports", importGroups, symbolNames) }},
		browseRow{label: fmt.Sprintf("Exports (%d)", countGroupedSymbols(exportGroups)), color: sectionRowColor, push: func() browseFrame { return elfSymbolGroupsFrame("exports", exportGroups, symbolNames) }},
	)
	return browseFrame{title: "elf -- esc/q back", rows: rows}
}

func countGroupedSymbols(groups []interface{}) int {
	n := 0
	for _, raw := range groups {
		g, ok := raw.(map[string]interface{})
		if !ok {
			continue
		}
		syms, _ := g["symbols"].([]interface{})
		n += len(syms)
	}
	return n
}

// elfSymbolGroupsFrame flattens the server's (type, binding, visibility[,
// section_index])-grouped symbol table into one row per symbol, resolving
// its interned name -- the grouping exists on the wire purely to avoid
// repeating those fields per symbol, so once resolved for display there's no
// reason to keep the TUI's list nested by group.
func elfSymbolGroupsFrame(kind string, groups []interface{}, symbolNames []string) browseFrame {
	var rows []browseRow
	for _, raw := range groups {
		g, ok := raw.(map[string]interface{})
		if !ok {
			continue
		}
		groupLabel := fmt.Sprintf("%s/%s/%s", stringField(g, "type"), stringField(g, "binding"), stringField(g, "visibility"))
		syms, _ := g["symbols"].([]interface{})
		for _, sraw := range syms {
			sym, ok := sraw.(map[string]interface{})
			if !ok {
				continue
			}
			entry := sym
			name := resolveSymbolName(entry, symbolNames)
			label := fmt.Sprintf("%-40s %s", name, groupLabel)
			rows = append(rows, browseRow{
				label: label,
				color: sectionRowColor,
				push: func() browseFrame {
					return fieldListFrame(fmt.Sprintf("elf %s symbol", strings.TrimSuffix(kind, "s")), withResolvedSymbolName(entry, name))
				},
			})
		}
	}
	return browseFrame{title: fmt.Sprintf("elf %s: %d symbols -- enter for detail, esc/q back", kind, len(rows)), rows: rows}
}

// resolveSymbolName looks a symbol's interned "name" index up in the
// snapshot-wide table; an out-of-range index (a malformed/mismatched
// response) falls back to showing the raw index rather than panicking or
// silently showing nothing.
func resolveSymbolName(sym map[string]interface{}, symbolNames []string) string {
	idx := intField(sym, "name")
	if idx >= 0 && idx < len(symbolNames) {
		return symbolNames[idx]
	}
	return fmt.Sprintf("#%d", idx)
}

// withResolvedSymbolName returns a copy of a symbol entry with its "name"
// index replaced by the resolved string, so the detail view shows the
// actual name instead of a meaningless catalog index.
func withResolvedSymbolName(sym map[string]interface{}, name string) map[string]interface{} {
	out := make(map[string]interface{}, len(sym))
	for k, v := range sym {
		out[k] = v
	}
	out["name"] = name
	return out
}

// fieldListFrame is an object browser: each of the object's own fields
// becomes a row via objectRows. title is also the breadcrumb prefix a
// nested folder row appends its own key to (e.g. "security" ->
// "security.credentials" -> "security.credentials.uids"), so the frame
// title always shows the full path you've drilled into.
func fieldListFrame(title string, fields map[string]interface{}) browseFrame {
	return browseFrame{title: title + " -- esc/q back", rows: objectRows(title, fields)}
}

// objectRows renders one level of an object's fields: a scalar or array
// value is a leaf row; a nested object becomes a "folder" row (colored,
// pushes a new fieldListFrame for that nested object) instead of a
// flattened dotted-path row, so drilling into any group works the same way
// at every depth -- "security" -> "credentials" -> "uids" -> "effective",
// not one row labeled "credentials.uids.effective". A mapping's own "file"
// id is dropped: the caller (memoryMapDetailFrame) adds its own dedicated
// "File"/"ELF" rows instead of showing the raw id. Every value here already
// arrived pre-decoded from the server (see server/pslistview.go) --
// formatFieldValue only renders it, it never interprets a raw code.
func objectRows(title string, fields map[string]interface{}) []browseRow {
	var leafKeys, folderKeys []string
	for k, v := range fields {
		if k == "file" {
			continue
		}
		if _, ok := v.(map[string]interface{}); ok {
			folderKeys = append(folderKeys, k)
		} else {
			leafKeys = append(leafKeys, k)
		}
	}
	sortFieldKeys(leafKeys)
	sortFieldKeys(folderKeys)

	rows := make([]browseRow, 0, len(leafKeys)+len(folderKeys))
	for _, k := range leafKeys {
		key := k
		v := fields[key]
		displayed := formatFieldValue(v)
		if isByteSizeField(key) {
			if s, ok := v.(string); ok {
				if human, ok := humanBytes(s); ok {
					displayed = human
				}
			}
		}
		rows = append(rows, browseRow{label: fmt.Sprintf("%s: %s", key, displayed), color: fieldValueColor(displayed)})
	}
	for _, k := range folderKeys {
		key := k
		nested, _ := fields[key].(map[string]interface{})
		rows = append(rows, browseRow{
			label: key,
			color: sectionRowColor,
			push:  func() browseFrame { return fieldListFrame(title+"."+key, nested) },
		})
	}
	return rows
}

// fieldOrderPriority ranks the field names worth always seeing first, in a
// meaningful order, instead of alphabetically -- e.g. a memory mapping's
// "start" must stay immediately followed by "end", not sorted apart by
// whatever else happens to start with a letter in between. Applies
// wherever a name in this list occurs, across every object shape in the
// schema; anything not listed falls back to alphabetical order after all
// of these. See sortFieldKeys/objectRows: fields (leaves) always sort
// before folders (nested objects) regardless of this list.
var fieldOrderPriority = []string{
	// status/type discriminators -- worth knowing before anything else
	"status", "collection_status", "path_status", "argv_status",
	"identity_status", "metadata_status", "type", "kind",
	// identifiers
	"pid", "tgid", "ppid", "fd",
	// paired address range -- start must stay immediately before end
	"start", "end",
	// the thing itself
	"path", "value", "name", "comm",
	// memory-map permission fields, kept together
	"permissions", "permission_limits", "sharing", "may_share", "deleted",
	// common ownership/identity
	"uid", "gid", "effective_uid", "effective_gid", "mode", "size", "inode",
	// timestamps, near the end of the leaf fields
	"mtime_ns", "ctime_ns", "access_time", "birth_time", "change_time", "modification_time",
	// errors last among leaves
	"error",
}

func fieldPriorityIndex(key string) int {
	for i, k := range fieldOrderPriority {
		if k == key {
			return i
		}
	}
	return len(fieldOrderPriority)
}

// sortFieldKeys orders keys by fieldOrderPriority, falling back to
// alphabetical for anything not in that list (and as the tiebreak among
// keys that are, though none of the priority entries collide today).
func sortFieldKeys(keys []string) {
	sort.SliceStable(keys, func(i, j int) bool {
		pi, pj := fieldPriorityIndex(keys[i]), fieldPriorityIndex(keys[j])
		if pi != pj {
			return pi < pj
		}
		return keys[i] < keys[j]
	})
}

// isByteSizeField reports whether a field name represents a byte quantity
// worth showing in human-scaled form. Detected by name since the server
// sends these as plain numeric strings (see strU64 in
// server/pslistview.go), the same JSON type as any other big-number
// field (inode, mtime, ...) that should *not* be scaled.
func isByteSizeField(key string) bool {
	switch key {
	case "size", "byte_length":
		return true
	}
	return strings.HasSuffix(key, "_bytes")
}

// humanBytes renders a byte count as e.g. "1.23MB (32324123124)" -- the
// human-scaled value alongside the exact raw one, so nothing is lost.
func humanBytes(raw string) (string, bool) {
	n, err := strconv.ParseUint(raw, 10, 64)
	if err != nil {
		return "", false
	}
	const (
		kb = 1 << 10
		mb = 1 << 20
		gb = 1 << 30
		tb = 1 << 40
	)
	var scaled string
	switch {
	case n >= tb:
		scaled = fmt.Sprintf("%.2fTB", float64(n)/tb)
	case n >= gb:
		scaled = fmt.Sprintf("%.2fGB", float64(n)/gb)
	case n >= mb:
		scaled = fmt.Sprintf("%.2fMB", float64(n)/mb)
	case n >= kb:
		scaled = fmt.Sprintf("%.2fKB", float64(n)/kb)
	default:
		scaled = fmt.Sprintf("%dB", n)
	}
	return fmt.Sprintf("%s (%d)", scaled, n), true
}

// formatFieldValue avoids Go's default %v scientific notation for large
// float64s -- every number here round-tripped through JSON from a Go
// integer, so it should print like one even though the free-form decode
// path only gives us float64.
func formatFieldValue(v interface{}) string {
	switch val := v.(type) {
	case float64:
		return strconv.FormatFloat(val, 'f', -1, 64)
	case string:
		return val
	case bool:
		return strconv.FormatBool(val)
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
