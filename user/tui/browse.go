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
//
// Every value read here already arrived from the server fully decoded (see
// server/pslistview.go): enum fields are strings, bitmasks are name arrays,
// timestamps are ISO-8601, permissions are ls -l-style strings. This file
// only arranges that data into frames/rows -- it never re-parses a raw wire
// code, matching the project's server/frontend responsibility boundary.

// pslistObjects bundles every per-object group map a task can link to, so
// taskDetailFrame and its children take one value instead of a growing list
// of positional map parameters.
type pslistObjects struct {
	identities     map[string]interface{}
	securities     map[string]interface{}
	contexts       map[string]interface{}
	resources      map[string]interface{}
	environments   map[string]interface{}
	memoryMaps     map[string]interface{}
	elfs           map[string]interface{}
	elfSymbolNames []string
	files          map[string]interface{}
}

func mapOrNil(m *map[string]interface{}) map[string]interface{} {
	if m == nil {
		return nil
	}
	return *m
}

func sliceOrNil(s *[]string) []string {
	if s == nil {
		return nil
	}
	return *s
}

func pslistFrame(snap api.ProcessSnapshot, sortMode int, filterText string) browseFrame {
	objs := pslistObjects{
		identities:     mapOrNil(snap.Identities),
		securities:     mapOrNil(snap.Securities),
		contexts:       mapOrNil(snap.Contexts),
		resources:      mapOrNil(snap.Resources),
		environments:   mapOrNil(snap.Environments),
		memoryMaps:     mapOrNil(snap.MemoryMaps),
		elfs:           mapOrNil(snap.Elfs),
		elfSymbolNames: sliceOrNil(snap.ElfSymbolNames),
		files:          mapOrNil(snap.Files),
	}

	tasks := snap.Tasks
	if filterText != "" {
		needle := strings.ToLower(filterText)
		filtered := make([]map[string]interface{}, 0, len(tasks))
		for _, t := range tasks {
			if strings.Contains(strings.ToLower(stringField(t, "comm")), needle) {
				filtered = append(filtered, t)
			}
		}
		tasks = filtered
	}
	tasks = append([]map[string]interface{}(nil), tasks...)
	sort.SliceStable(tasks, func(i, j int) bool {
		switch sortMode {
		case sortByComm:
			return stringField(tasks[i], "comm") < stringField(tasks[j], "comm")
		case sortByState:
			return stringField(tasks[i], "state") < stringField(tasks[j], "state")
		default:
			return intField(tasks[i], "pid") < intField(tasks[j], "pid")
		}
	})

	rows := make([]browseRow, len(tasks))
	for i, raw := range tasks {
		t := raw
		state := stringField(t, "state")
		label := fmt.Sprintf("%-7d %-20s %-13s %s",
			intField(t, "pid"), stringField(t, "comm"), state, stringField(t, "type"))
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

func taskDetailFrame(task map[string]interface{}, objs pslistObjects) browseFrame {
	pid := intField(task, "pid")
	comm := stringField(task, "comm")

	rows := []browseRow{
		{label: fmt.Sprintf("pid: %d", pid)},
		{label: fmt.Sprintf("tgid: %d", intField(task, "tgid"))},
		{label: fmt.Sprintf("ppid: %d", intField(task, "ppid"))},
		{label: fmt.Sprintf("comm: %s", comm)},
		{label: fmt.Sprintf("state: %s", stringField(task, "state"))},
		{label: fmt.Sprintf("type: %s", stringField(task, "type"))},
	}

	if id, ok := groupRef(task, "identity_id"); ok {
		if obj, ok := lookupObject(objs.identities, id); ok {
			rows = append(rows, browseRow{label: "Identity", color: sectionRowColor, push: func() browseFrame { return fieldListFrame("identity", obj) }})
		}
	}
	if id, ok := groupRef(task, "security_id"); ok {
		if obj, ok := lookupObject(objs.securities, id); ok {
			rows = append(rows, browseRow{label: "Security", color: sectionRowColor, push: func() browseFrame { return fieldListFrame("security", obj) }})
		}
	}
	if id, ok := groupRef(task, "context_id"); ok {
		if obj, ok := lookupObject(objs.contexts, id); ok {
			rows = append(rows, browseRow{label: "Context", color: sectionRowColor, push: func() browseFrame { return fieldListFrame("context", obj) }})
		}
	}
	if id, ok := groupRef(task, "resources_id"); ok {
		if obj, ok := lookupObject(objs.resources, id); ok {
			rows = append(rows, browseRow{label: "Resources", color: sectionRowColor, push: func() browseFrame { return fieldListFrame("resources", obj) }})
		}
	}
	if id, ok := groupRef(task, "environment_id"); ok {
		if obj, ok := lookupObject(objs.environments, id); ok {
			rows = append(rows, browseRow{label: "Environment", color: sectionRowColor, push: func() browseFrame { return environmentFrame(obj) }})
		}
	}
	if id, ok := groupRef(task, "memory_maps_id"); ok {
		if obj, ok := lookupObject(objs.memoryMaps, id); ok {
			rows = append(rows, browseRow{label: "Memory Maps", color: sectionRowColor, push: func() browseFrame { return memoryMapsFrame(obj, objs.elfs, objs.elfSymbolNames) }})
		}
	}
	if id, ok := groupRef(task, "files_id"); ok {
		if obj, ok := lookupObject(objs.files, id); ok {
			rows = append(rows, browseRow{label: "Files", color: sectionRowColor, push: func() browseFrame { return fdListFrame(obj) }})
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

// environmentFrame shows a task's captured environment as individual
// KEY=VALUE leaf rows -- the server hands these back as a real JSON object
// (see viewEnvironment), no client-side splitting needed.
func environmentFrame(obj map[string]interface{}) browseFrame {
	rows := []browseRow{
		{label: fmt.Sprintf("availability_status: %s", stringField(obj, "availability_status"))},
	}
	env, _ := obj["environment"].(map[string]interface{})
	keys := make([]string, 0, len(env))
	for k := range env {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		rows = append(rows, browseRow{label: fmt.Sprintf("%s=%s", k, formatFieldValue(env[k]))})
	}
	return browseFrame{title: fmt.Sprintf("environment: %d entries -- esc/q back", len(keys)), rows: rows}
}

// memoryMapsFrame lists a task's memory mappings; enter drills into one
// mapping's fields, which itself gains an "ELF" row when the mapping is
// backed by a file mapple has captured ELF metadata for.
func memoryMapsFrame(obj map[string]interface{}, elfs map[string]interface{}, symbolNames []string) browseFrame {
	mappingsRaw, _ := obj["mappings"].([]interface{})
	rows := make([]browseRow, 0, len(mappingsRaw))
	for _, raw := range mappingsRaw {
		m, ok := raw.(map[string]interface{})
		if !ok {
			continue
		}
		mapping := m
		start := stringField(mapping, "start_address")
		end := stringField(mapping, "end_address")
		perms := stringField(mapping, "permissions")
		name := stringField(mapping, "path")
		if name == "" {
			name = stringField(mapping, "name")
		}
		if name == "" {
			name = "[" + stringField(mapping, "name_kind") + "]"
		}
		label := fmt.Sprintf("%s-%s %s %s", start, end, perms, name)
		rows = append(rows, browseRow{
			label: label,
			color: sectionRowColor,
			push:  func() browseFrame { return memoryMapDetailFrame(mapping, elfs, symbolNames) },
		})
	}
	return browseFrame{title: fmt.Sprintf("memory maps: %d mappings -- enter for detail, esc/q back", len(rows)), rows: rows}
}

func memoryMapDetailFrame(mapping, elfs map[string]interface{}, symbolNames []string) browseFrame {
	rows := flattenedRows(mapping)
	if id, ok := groupRef(mapping, "elf_object_id"); ok {
		if obj, ok := lookupObject(elfs, id); ok {
			rows = append(rows, browseRow{label: "ELF", color: sectionRowColor, push: func() browseFrame { return elfFrame(obj, symbolNames) }})
		}
	}
	start := stringField(mapping, "start_address")
	end := stringField(mapping, "end_address")
	return browseFrame{title: fmt.Sprintf("mapping %s-%s -- esc/q back", start, end), rows: rows}
}

// elfFrame shows one ELF file's header metadata plus its string, import and
// export tables as separate pushes rather than flattened inline, since a
// heavily-linked binary can have thousands of symbols. Import/export symbol
// names are interned snapshot-wide (see server/pslistview.go's
// elfSymbolCatalog) -- symbolNames is that shared table, resolved here by
// index rather than the TUI re-parsing anything.
func elfFrame(obj map[string]interface{}, symbolNames []string) browseFrame {
	stringsRaw, _ := obj["strings"].([]interface{})
	imports, _ := obj["imports"].(map[string]interface{})
	importGroups, _ := imports["groups"].([]interface{})
	exports, _ := obj["exports"].(map[string]interface{})
	exportGroups, _ := exports["groups"].([]interface{})

	fields := make(map[string]interface{}, len(obj))
	for k, v := range obj {
		if k == "strings" || k == "imports" || k == "exports" {
			continue
		}
		fields[k] = v
	}
	rows := flattenedRows(fields)
	rows = append(rows,
		browseRow{label: fmt.Sprintf("Strings (%d)", len(stringsRaw)), color: sectionRowColor, push: func() browseFrame { return elfStringListFrame(stringsRaw) }},
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

func elfStringListFrame(items []interface{}) browseFrame {
	rows := make([]browseRow, 0, len(items))
	for _, raw := range items {
		s, ok := raw.(map[string]interface{})
		if !ok {
			continue
		}
		entry := s
		kind := stringField(entry, "string_kind")
		label := fmt.Sprintf("%-12s %s", kind, stringField(entry, "value"))
		rows = append(rows, browseRow{
			label: label,
			color: sectionRowColor,
			push: func() browseFrame {
				return fieldListFrame(fmt.Sprintf("elf string %d", intField(entry, "string_index")), entry)
			},
		})
	}
	return browseFrame{title: fmt.Sprintf("elf strings: %d -- enter for detail, esc/q back", len(rows)), rows: rows}
}

// fieldListFrame is a leaf: flat key/value rows, nested objects flattened
// with dotted keys (e.g. an fd's "details.stat.mode").
func fieldListFrame(title string, fields map[string]interface{}) browseFrame {
	rows := flattenedRows(fields)
	return browseFrame{title: title + " -- esc/q back", rows: rows}
}

// flattenedRows renders every field as a leaf row, flattening nested
// objects with dotted keys. elf_object_id is dropped: where it matters
// (memory_map) the caller adds its own dedicated "ELF" row instead of
// showing the raw id. Every value here already arrived pre-decoded from the
// server (see server/pslistview.go) -- formatFieldValue only renders it,
// it never interprets a raw code.
func flattenedRows(fields map[string]interface{}) []browseRow {
	flat := map[string]interface{}{}
	flattenFields("", fields, flat)
	keys := make([]string, 0, len(flat))
	for k := range flat {
		if k == "elf_object_id" || strings.HasSuffix(k, ".elf_object_id") {
			continue
		}
		keys = append(keys, k)
	}
	sort.Strings(keys)
	rows := make([]browseRow, len(keys))
	for i, k := range keys {
		displayed := formatFieldValue(flat[k])
		rows[i] = browseRow{label: fmt.Sprintf("%s: %s", k, displayed), color: fieldValueColor(displayed)}
	}
	return rows
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
