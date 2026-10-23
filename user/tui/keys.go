package tui

import (
	"context"
	"fmt"
	"sort"
	"strconv"
	"strings"
	"unicode/utf8"

	tea "charm.land/bubbletea/v2"

	"github.com/GhostDog788/Mapple/user/api"
)

func (m Model) handleKey(msg tea.KeyPressMsg) (tea.Model, tea.Cmd) {
	if msg.String() == "ctrl+c" {
		return m.quit()
	}
	if len(m.frames) > 0 {
		return m.handleBrowseKey(msg)
	}
	if m.historySearching {
		return m.handleHistorySearchKey(msg)
	}
	return m.handleReplKey(msg)
}

// --- browse mode: up/down move the cursor, enter selects, esc/q backs out ---

func (m Model) handleBrowseKey(msg tea.KeyPressMsg) (tea.Model, tea.Cmd) {
	if m.searching {
		return m.handleSearchKey(msg)
	}
	top := &m.frames[len(m.frames)-1]
	switch msg.String() {
	case "up", "k":
		top.cursor--
		top.clampCursor()
		m.refreshBrowseView()
		return m, nil
	case "down", "j":
		top.cursor++
		top.clampCursor()
		m.refreshBrowseView()
		return m, nil
	case "left", "h":
		m.viewport.ScrollLeft(4)
		return m, nil
	case "right", "l":
		m.viewport.ScrollRight(4)
		return m, nil
	case "pgup":
		m.viewport.PageUp()
		return m, nil
	case "pgdown":
		m.viewport.PageDown()
		return m, nil
	case "/":
		if top.kind == framePslist {
			m.searching = true
			m.input.SetValue("")
			m.input.Prompt = "/"
			m.input.Placeholder = "process name"
			m.input.Focus()
		}
		return m, nil
	case "s":
		if top.kind == framePslist {
			m.pslistSortMode = (m.pslistSortMode + 1) % 3
			m.replaceTopFrame(pslistFrame(*m.lastSnapshot, m.pslistSortMode, m.pslistFilter))
		}
		return m, nil
	case "space":
		if top.kind == frameGroupChecklist {
			m.toggleChecklistRow(top.cursor)
		}
		return m, nil
	case "f":
		if top.kind == frameGroupChecklist {
			return m.startPslistFetchSelected()
		}
		return m, nil
	case "esc", "q":
		if m.popFrame() {
			m.input.Focus()
		}
		return m, nil
	case "enter":
		if len(top.rows) == 0 {
			return m, nil
		}
		if top.kind == frameGroupChecklist {
			if top.cursor < len(pslistGroupNames) {
				m.toggleChecklistRow(top.cursor)
				return m, nil
			}
			return m.startPslistFetchSelected()
		}
		row := top.rows[top.cursor]
		switch {
		case row.connect != nil:
			m.connectTo(*row.connect)
			m.popFrame()
			m.input.Focus()
		case row.viewHistory != nil:
			m.lastSnapshot = &row.viewHistory.snapshot
			m.pslistSortMode = sortByPID
			m.pslistFilter = ""
			m.pushFrame(pslistFrame(row.viewHistory.snapshot, m.pslistSortMode, m.pslistFilter))
		case row.recall != nil:
			m.popFrame()
			m.input.Focus()
			m.input.SetValue(*row.recall)
			m.input.CursorEnd()
		case row.push != nil:
			m.pushFrame(row.push())
		}
		return m, nil
	}
	return m, nil
}

// toggleChecklistRow flips the group at row index cursor and rebuilds the
// checklist frame in place, restoring cursor afterward -- replaceTopFrame
// on its own would reset it to 0 (groupChecklistFrame always builds a fresh
// browseFrame at cursor 0), which would make repeated toggles unusable
// (every space/enter press would snap you back to the top row).
func (m *Model) toggleChecklistRow(cursor int) {
	if cursor < 0 || cursor >= len(pslistGroupNames) || m.pslistGroupSelection == nil {
		return
	}
	group := pslistGroupNames[cursor]
	m.pslistGroupSelection[group] = !m.pslistGroupSelection[group]
	m.replaceTopFrame(groupChecklistFrame(m.pslistGroupSelection))
	m.frames[len(m.frames)-1].cursor = cursor
	m.refreshBrowseView()
}

// startPslistFetchSelected closes the group checklist and fires the pslist
// request with only the checked groups.
func (m Model) startPslistFetchSelected() (tea.Model, tea.Cmd) {
	if m.current == nil {
		m.printLine("not connected -- `session connect` or `session create` first")
		return m, nil
	}
	var groups []api.PslistGroup
	for _, g := range pslistGroupNames {
		if m.pslistGroupSelection[g] {
			groups = append(groups, api.PslistGroup(g))
		}
	}
	if len(groups) == 0 {
		m.printError("select at least one group first (space toggles, f fetches)")
		return m, nil
	}
	m.popFrame()
	m.input.Focus()
	return m, m.fetchPslist(m.current.Id, groups)
}

// handleSearchKey captures the string typed after "/" into m.input --
// reused rather than a dedicated textinput.Model since it's idle during
// browse mode anyway. Enter applies it as the pslist Comm filter in place
// (replaceTopFrame, not push, so it doesn't stack a duplicate); esc cancels
// without changing the filter.
func (m Model) handleSearchKey(msg tea.KeyPressMsg) (tea.Model, tea.Cmd) {
	switch msg.String() {
	case "esc":
		m.searching = false
		m.input.Blur()
		m.input.Prompt = "> "
		m.input.Placeholder = replPlaceholder
		return m, nil
	case "enter":
		m.searching = false
		m.input.Blur()
		m.input.Prompt = "> "
		m.input.Placeholder = replPlaceholder
		m.pslistFilter = strings.TrimSpace(m.input.Value())
		m.replaceTopFrame(pslistFrame(*m.lastSnapshot, m.pslistSortMode, m.pslistFilter))
		return m, nil
	}
	var cmd tea.Cmd
	m.input, cmd = m.input.Update(msg)
	return m, cmd
}

// --- command-line mode: a small REPL (session/command/sort/filter/help/quit) ---

func (m Model) handleReplKey(msg tea.KeyPressMsg) (tea.Model, tea.Cmd) {
	switch msg.String() {
	case "pgup":
		m.viewport.PageUp()
		return m, nil
	case "pgdown":
		m.viewport.PageDown()
		return m, nil
	case "home":
		m.viewport.GotoTop()
		return m, nil
	case "end":
		m.viewport.GotoBottom()
		return m, nil
	case "tab":
		m.completeInput()
		return m, nil
	case "up":
		m.navigateHistory(-1)
		return m, nil
	case "down":
		m.navigateHistory(1)
		return m, nil
	case "ctrl+r":
		m.historySearching = true
		m.historySearchOrigInput = m.input.Value()
		m.historySearchQuery = ""
		m.historySearchIndex = 0
		m.applyHistorySearch()
		return m, nil
	case "enter":
		line := strings.TrimSpace(m.input.Value())
		m.input.SetValue("")
		return m.submitLine(line)
	}
	var cmd tea.Cmd
	m.input, cmd = m.input.Update(msg)
	return m, cmd
}

// submitLine records and dispatches a completed REPL line -- shared by the
// normal enter key and history-search's enter (which accepts a recalled
// command and runs it immediately, same as a real shell's ctrl+r).
func (m Model) submitLine(line string) (tea.Model, tea.Cmd) {
	if line == "" {
		return m, nil
	}
	m.recordHistory(line)
	m.printLine("> " + line)
	if m.pending != nil {
		return m.advanceWizard(line)
	}
	return m.dispatch(line)
}

// --- history search mode: ctrl+r, shell-style incremental reverse search ---

// handleHistorySearchKey implements a ctrl+r reverse-i-search: typing
// narrows to history entries containing the typed substring (most recent
// match first), repeated ctrl+r cycles to the next older match, enter
// accepts and runs the match immediately (matching a real shell's ctrl+r),
// esc restores whatever was typed before the search started. Any other key
// (arrows, tab, ...) leaves the current match loaded in the input and
// drops back to normal editing.
func (m Model) handleHistorySearchKey(msg tea.KeyPressMsg) (tea.Model, tea.Cmd) {
	switch {
	case msg.String() == "ctrl+r":
		m.historySearchIndex++
		m.applyHistorySearch()
		return m, nil
	case msg.String() == "esc":
		m.endHistorySearch()
		m.input.SetValue(m.historySearchOrigInput)
		m.input.CursorEnd()
		return m, nil
	case msg.String() == "enter":
		line := strings.TrimSpace(m.input.Value())
		m.endHistorySearch()
		m.input.SetValue("")
		return m.submitLine(line)
	case msg.String() == "backspace":
		if len(m.historySearchQuery) > 0 {
			_, size := utf8.DecodeLastRuneInString(m.historySearchQuery)
			m.historySearchQuery = m.historySearchQuery[:len(m.historySearchQuery)-size]
		}
		m.historySearchIndex = 0
		m.applyHistorySearch()
		return m, nil
	case msg.Text != "":
		m.historySearchQuery += msg.Text
		m.historySearchIndex = 0
		m.applyHistorySearch()
		return m, nil
	default:
		m.endHistorySearch()
		return m, nil
	}
}

// applyHistorySearch recomputes the current match for
// historySearchQuery/historySearchIndex and reflects it in the input box
// behind a bash-style "(reverse-i-search)`query': " prompt.
func (m *Model) applyHistorySearch() {
	m.input.Prompt = fmt.Sprintf("(reverse-i-search)`%s': ", m.historySearchQuery)
	matches := matchingHistory(m.history, m.historySearchQuery)
	if len(matches) == 0 {
		m.input.SetValue("")
		return
	}
	if m.historySearchIndex >= len(matches) {
		m.historySearchIndex = len(matches) - 1
	}
	if m.historySearchIndex < 0 {
		m.historySearchIndex = 0
	}
	m.input.SetValue(matches[m.historySearchIndex])
	m.input.CursorEnd()
}

func (m *Model) endHistorySearch() {
	m.historySearching = false
	m.input.Prompt = "> "
	m.input.Placeholder = replPlaceholder
}

// --- tab completion ---

var replGrammar = map[int]map[string][]string{
	0: {"": {"session", "command", "log", "history", "help", "quit", "exit"}},
	1: {
		"session": {"list", "connect", "create", "delete", "current", "profiles", "save-profile", "help"},
		"command": {"info", "echo", "getfile", "pslist", "help"},
		"log":     {"client", "server", "module", "help"},
	},
}

// replGrammar2 is keyed by "word0 word1" for the verbs that need a third
// completion level: pslist's own "history", and "help" for every "command"
// subverb (session's own subverbs take an id/no argument, so "session help"
// is already reachable at word index 1 above).
var replGrammar2 = map[string][]string{
	"command pslist":  {"history", "groups", "help"},
	"command info":    {"help"},
	"command echo":    {"help"},
	"command getfile": {"help"},
}

func (m *Model) completeInput() {
	text := m.input.Value()
	trailingSpace := strings.HasSuffix(text, " ")
	tokens := strings.Fields(text)

	wordIndex := len(tokens)
	prefix := ""
	if !trailingSpace && len(tokens) > 0 {
		wordIndex--
		prefix = tokens[len(tokens)-1]
	}

	var candidates []string
	switch {
	case wordIndex == 0:
		candidates = replGrammar[0][""]
	case wordIndex == 1 && len(tokens) > 0:
		candidates = replGrammar[1][tokens[0]]
	case wordIndex == 2 && len(tokens) > 1:
		candidates = replGrammar2[tokens[0]+" "+tokens[1]]
	}

	var matches []string
	for _, c := range candidates {
		if strings.HasPrefix(c, prefix) {
			matches = append(matches, c)
		}
	}

	switch len(matches) {
	case 0:
		return
	case 1:
		base := tokens
		if len(base) > 0 && !trailingSpace {
			base = base[:len(base)-1]
		}
		newText := strings.Join(append(base, matches[0]), " ") + " "
		m.input.SetValue(newText)
		m.input.CursorEnd()
	default:
		m.printLine("completions: " + strings.Join(matches, "  "))
	}
}

// --- command dispatch ---

func (m Model) dispatch(line string) (tea.Model, tea.Cmd) {
	fields := strings.Fields(line)
	verb := fields[0]
	rest := fields[1:]

	switch verb {
	case "help":
		if len(rest) > 0 {
			m.printError("unexpected argument %q", rest[0])
			return m, nil
		}
		m.printHelp()
		return m, nil
	case "quit", "exit":
		if len(rest) > 0 {
			m.printError("unexpected argument %q", rest[0])
			return m, nil
		}
		return m.quit()
	case "session":
		return m.dispatchSession(rest)
	case "command":
		if len(rest) > 0 && rest[0] == "getfile" {
			command := strings.TrimSpace(strings.TrimPrefix(strings.TrimSpace(line), verb))
			args, err := getfileArgs(command)
			if err != nil {
				m.printError("%v", err)
				return m, nil
			}
			return m.dispatchCommand(args)
		}
		return m.dispatchCommand(rest)
	case "log":
		return m.dispatchLog(rest)
	case "history":
		if len(rest) > 0 {
			m.printError("unexpected argument %q", rest[0])
			return m, nil
		}
		if len(m.history) == 0 {
			m.printLine("no command history yet")
			return m, nil
		}
		m.pushFrame(historyFrame(m.history))
		return m, nil
	default:
		m.printError("unknown command %q -- try `help`", verb)
		return m, nil
	}
}

func (m *Model) printHelp() {
	m.printLine(strings.Join([]string{
		"session          -- manage sessions -- see `session help`",
		"command          -- run a command against the connected session -- see `command help`",
		"log              -- view retained logs -- see `log help`",
		"history          -- browse previously submitted commands, enter to recall",
		"help             -- this message",
		"quit, exit       -- exit the TUI",
		"",
		"any verb above takes a trailing `help` for more detail (`session help`,",
		"`command help`), and each command in turn has its own (`command pslist help`).",
		"",
		"ctrl+r           -- incremental reverse history search: type to filter,",
		"                    ctrl+r again for an older match, enter runs it, esc cancels",
	}, "\n"))
}

// dispatchLog handles `log client|server|module`, viewing retained events
// for that source. Only "client" (this TUI process's own
// printError/printSuccess history) is wired up today; "server" and
// "module" are recognized sources that report "not available yet" until a
// follow-up task adds real retention/fetching for them (see the plan for
// why: the daemon keeps no log history beyond stdout/journald today, and
// the wire protocol has no log-fetch command at all).
func (m Model) dispatchLog(args []string) (tea.Model, tea.Cmd) {
	if len(args) == 0 {
		m.printLogHelp()
		return m, nil
	}
	if len(args) > 1 {
		m.printError("unexpected argument %q", args[1])
		return m, nil
	}
	switch args[0] {
	case "client":
		m.pushFrame(clientLogFrame(m.clientLog))
	case "server":
		m.printError("server logs: not available yet")
	case "module":
		m.printError("module logs: not available yet")
	case "help":
		m.printLogHelp()
	default:
		m.printError("unknown log source %q -- try `log help`", args[0])
	}
	return m, nil
}

func (m *Model) printLogHelp() {
	m.printLine(strings.Join([]string{
		"usage: log client|server|module",
		"",
		"client  -- this TUI's own retained error/success history",
		"server  -- the mappled daemon's logs (not available yet)",
		"module  -- the kernel agent's logs (not available yet)",
	}, "\n"))
}

func (m *Model) printSessionHelp() {
	m.printLine(strings.Join([]string{
		"session list                    -- browse sessions in a list; enter connects, esc/q cancels",
		"session connect <id-prefix>     -- connect without opening the browser;",
		"                                   <id-prefix> just needs to be unambiguous",
		"session create                  -- wizard: pick tcp or chardev, then its",
		"                                   destination (host:port, or a device path) --",
		"                                   or type a saved module profile's name instead",
		"session create <profile>        -- skip the wizard, connect using a saved module",
		"                                   profile directly (see `session profiles`)",
		"session profiles                -- list module profiles saved in the config file's",
		"                                   \"module_profiles\" section (name -> transport + destination)",
		"session save-profile <name>     -- save the current session's own destination as a module",
		"                                   profile under <name>, for later `session create <name>`",
		"session current                 -- show the connected session's full details (untruncated id,",
		"                                   destination, state, created/expires times, info)",
		"session delete <id-prefix>      -- delete a session by id prefix",
	}, "\n"))
}

func (m *Model) printCommandHelp() {
	m.printLine(strings.Join([]string{
		"command info                    -- session/protocol info -- see `command info help`",
		"command echo <text>             -- round-trip text through the module -- see `command echo help`",
		"command getfile <remote> <local> -- stream a file to a local destination",
		"command pslist                  -- process/thread snapshot -- see `command pslist help`",
		"",
		"all commands require a connected session (`session connect`/`session create` first).",
	}, "\n"))
}

func (m *Model) printCommandInfoHelp() {
	m.printLine(strings.Join([]string{
		"command info",
		"  Fetches session_id, max_sessions, and supported_commands (the wire",
		"  protocol commands the connected kernel module supports) for the",
		"  current session. Takes no arguments.",
	}, "\n"))
}

func (m *Model) printCommandEchoHelp() {
	m.printLine(strings.Join([]string{
		"command echo <text>",
		"  Sends <text> to the kernel module and prints back whatever it echoes.",
		"  Mainly a connectivity/sanity check for the session -- the reply should",
		"  match what was sent.",
	}, "\n"))
}

func (m *Model) printCommandGetfileHelp() {
	m.printLine("command getfile <remote-path> <local-path>\n  Downloads an ordinary file from the module to a local file.\n  Remote path must be absolute, at most 4096 UTF-8 bytes, without NULs.\n  Quote paths containing spaces with double quotes (JSON escapes).\n  Existing files are replaced after a successful download; failures leave them intact.\n  Example: command getfile /etc/hostname ./hostname.txt\n  command getfile cancel -- cancel the active download; its module session may need reconnecting.\n  Downloads are not snapshots of files being modified concurrently.")
}

func (m *Model) printCommandPslistHelp() {
	m.printLine(strings.Join([]string{
		"command pslist                  -- fetch a fresh process/thread snapshot, requesting every",
		"                                   group the module supports (kept in history, see below)",
		"command pslist groups           -- opens a checklist of the 10 pslist groups; space/enter",
		"                                   toggles the group under the cursor, f (or enter on the",
		"                                   Fetch row) requests only the checked ones, esc/q cancels",
		"command pslist history          -- browse up to the last 5 pslist results;",
		"                                   enter views one without re-fetching",
		"",
		"sort and filter are only available while actually browsing a pslist",
		"result (not as typed commands), so they always act on what's on screen:",
		"s        -- cycle sort: pid -> comm -> state -> pid ...",
		"/        -- filter by process name; enter applies, esc cancels, an",
		"            empty filter (just press enter) clears it",
	}, "\n"))
}

func (m Model) dispatchSession(args []string) (tea.Model, tea.Cmd) {
	if len(args) == 0 {
		m.printLine("usage: session list|connect <id>|create [profile]|delete <id>|current|profiles|save-profile <name>")
		return m, nil
	}
	if args[0] == "help" {
		if len(args) > 1 {
			m.printError("unexpected argument %q", args[1])
			return m, nil
		}
		m.printSessionHelp()
		return m, nil
	}
	switch args[0] {
	case "list":
		if len(args) > 1 {
			m.printError("unexpected argument %q", args[1])
			return m, nil
		}
		return m, m.loadSessions()
	case "connect":
		if len(args) < 2 {
			m.printLine("usage: session connect <id-prefix>")
			return m, nil
		}
		if len(args) > 2 {
			m.printError("unexpected argument %q", args[2])
			return m, nil
		}
		sess, err := m.resolveSession(args[1])
		if err != nil {
			m.printError("%v", err)
			return m, nil
		}
		m.connectTo(sess)
		return m, nil
	case "create":
		if len(args) > 1 {
			if len(args) > 2 {
				m.printError("unexpected argument %q", args[2])
				return m, nil
			}
			return m.createFromProfile(args[1])
		}
		m.pending = &wizard{step: 0}
		if len(m.moduleProfiles) > 0 {
			m.printLine("transport (tcp/chardev), or a saved profile name (see `session profiles`):")
		} else {
			m.printLine("transport (tcp/chardev):")
		}
		return m, nil
	case "profiles":
		if len(args) > 1 {
			m.printError("unexpected argument %q", args[1])
			return m, nil
		}
		return m.printModuleProfiles()
	case "current":
		if len(args) > 1 {
			m.printError("unexpected argument %q", args[1])
			return m, nil
		}
		return m.printCurrentSession()
	case "save-profile":
		if len(args) < 2 {
			m.printLine("usage: session save-profile <name>")
			return m, nil
		}
		if len(args) > 2 {
			m.printError("unexpected argument %q", args[2])
			return m, nil
		}
		return m.saveCurrentAsProfile(args[1])
	case "delete":
		if len(args) < 2 {
			m.printLine("usage: session delete <id-prefix>")
			return m, nil
		}
		if len(args) > 2 {
			m.printError("unexpected argument %q", args[2])
			return m, nil
		}
		sess, err := m.resolveSession(args[1])
		if err != nil {
			m.printError("%v", err)
			return m, nil
		}
		return m, m.deleteSession(sess.Id)
	default:
		m.printError("unknown session subcommand %q", args[0])
		return m, nil
	}
}

// createFromProfile looks up a saved module profile by name and creates a
// session against it directly, skipping the interactive transport/
// destination wizard entirely.
func (m Model) createFromProfile(name string) (tea.Model, tea.Cmd) {
	profile, ok := m.moduleProfiles[name]
	if !ok {
		m.printError("no such module profile %q (see `session profiles`)", name)
		return m, nil
	}
	dest, err := profile.Destination()
	if err != nil {
		m.printError("%v", err)
		return m, nil
	}
	return m, m.createSession(dest)
}

// saveCurrentAsProfile writes the currently-connected session's own
// destination into the config file's "module_profiles" section under name,
// so a later `session create <name>` (or typing name at the wizard's
// transport prompt) reconnects to the same place without retyping it.
func (m Model) saveCurrentAsProfile(name string) (tea.Model, tea.Cmd) {
	if m.current == nil {
		m.printLine("not connected -- `session connect` or `session create` first")
		return m, nil
	}
	if m.configPath == "" {
		m.printError("no config file path known for this session -- can't save a profile")
		return m, nil
	}
	profile := ModuleProfileFromDestination(m.current.Destination)
	if err := SaveModuleProfile(m.configPath, name, profile); err != nil {
		m.printError("%v", err)
		return m, nil
	}
	if m.moduleProfiles == nil {
		m.moduleProfiles = map[string]ModuleProfile{}
	}
	m.moduleProfiles[name] = profile
	m.printSuccess("saved module profile %q (%s) to %s", name, profile, m.configPath)
	return m, nil
}

// printCurrentSession shows the connected session's full, untruncated
// details -- the status bar only ever shows the shortened 8-char id
// (shortID), so this is the way to see the real one (e.g. to pass to
// another tool, or to double check which session you're actually on).
func (m Model) printCurrentSession() (tea.Model, tea.Cmd) {
	if m.current == nil {
		m.printLine("not connected -- `session connect` or `session create` first")
		return m, nil
	}
	s := m.current
	lines := []string{
		fmt.Sprintf("id:          %s", s.Id),
		fmt.Sprintf("state:       %s", s.State),
		fmt.Sprintf("destination: %s", destSummary(s.Destination)),
		fmt.Sprintf("created_at:  %s", s.CreatedAt.Format("2006-01-02 15:04:05 MST")),
		fmt.Sprintf("expires_at:  %s", s.ExpiresAt.Format("2006-01-02 15:04:05 MST")),
	}
	if s.Info != nil {
		lines = append(lines,
			fmt.Sprintf("session_id:  %d", s.Info.SessionId),
			fmt.Sprintf("max_sessions: %d", s.Info.MaxSessions),
			fmt.Sprintf("supported_commands: %s", commandNames(s.Info.SupportedCommands)),
		)
	}
	m.printLine(strings.Join(lines, "\n"))
	return m, nil
}

func (m Model) printModuleProfiles() (tea.Model, tea.Cmd) {
	if len(m.moduleProfiles) == 0 {
		m.printLine("no module profiles configured -- add a \"module_profiles\" section to your config file")
		return m, nil
	}
	names := make([]string, 0, len(m.moduleProfiles))
	for name := range m.moduleProfiles {
		names = append(names, name)
	}
	sort.Strings(names)
	lines := make([]string, 0, len(names))
	for _, name := range names {
		lines = append(lines, fmt.Sprintf("%-16s %s", name, m.moduleProfiles[name]))
	}
	m.printLine(strings.Join(lines, "\n"))
	return m, nil
}

func (m Model) resolveSession(prefix string) (api.Session, error) {
	var matches []api.Session
	for _, s := range m.sessions {
		if s.Id == prefix {
			return s, nil
		}
		if strings.HasPrefix(s.Id, prefix) {
			matches = append(matches, s)
		}
	}
	switch len(matches) {
	case 0:
		return api.Session{}, fmt.Errorf("no session matches %q (try `session list` first)", prefix)
	case 1:
		return matches[0], nil
	default:
		return api.Session{}, fmt.Errorf("%q matches %d sessions, be more specific", prefix, len(matches))
	}
}

func (m Model) dispatchCommand(args []string) (tea.Model, tea.Cmd) {
	if len(args) == 0 {
		m.printLine("usage: command info|echo <text>|getfile <remote> <local>|pslist [groups|history]")
		return m, nil
	}
	if args[0] == "help" {
		if len(args) > 1 {
			m.printError("unexpected argument %q", args[1])
			return m, nil
		}
		m.printCommandHelp()
		return m, nil
	}
	// Help for a specific subverb, e.g. "command pslist help" -- checked
	// before the connection gate below, same reasoning as pslist history:
	// help doesn't need a live session either.
	if len(args) > 1 && args[1] == "help" {
		if len(args) > 2 {
			m.printError("unexpected argument %q", args[2])
			return m, nil
		}
		switch args[0] {
		case "info":
			m.printCommandInfoHelp()
		case "echo":
			m.printCommandEchoHelp()
		case "getfile":
			m.printCommandGetfileHelp()
		case "pslist":
			m.printCommandPslistHelp()
		default:
			m.printError("unknown command %q", args[0])
		}
		return m, nil
	}
	if args[0] == "getfile" && len(args) == 2 && args[1] == "cancel" {
		if m.downloadCancel != nil {
			m.downloadCancel()
			m.printLine("Canceling download...")
		} else {
			m.printLine("no download is running")
		}
		return m, nil
	}
	// pslist history browses cached results client-side, same as sort/filter
	// below -- no live session needed, so it's checked before the
	// connection gate that every other "command" subverb requires.
	if args[0] == "pslist" && len(args) > 1 && args[1] == "history" {
		if len(args) > 2 {
			m.printError("unexpected argument %q", args[2])
			return m, nil
		}
		return m.openPslistHistory()
	}
	if m.current == nil {
		m.printLine("not connected -- `session connect <id>` or `session create` first")
		return m, nil
	}
	id := m.current.Id
	switch args[0] {
	case "info":
		if len(args) > 1 {
			m.printError("unexpected argument %q", args[1])
			return m, nil
		}
		return m, m.fetchInfo(id)
	case "echo":
		text := strings.Join(args[1:], " ")
		return m, m.sendEcho(id, text)
	case "getfile":
		if len(args) != 3 || args[1] == "" || args[2] == "" {
			m.printError("usage: command getfile <remote-path> <local-path>")
			return m, nil
		}
		if m.downloadCancel != nil {
			m.printError("a download is already running; use command getfile cancel")
			return m, nil
		}
		ctx, cancel := context.WithCancel(context.Background())
		m.downloadCancel = cancel
		m.printLine("Downloading to " + args[2] + " (command getfile cancel to cancel)")
		return m, m.fetchGetfile(ctx, id, args[1], args[2])
	case "pslist":
		if len(args) > 1 {
			if args[1] == "groups" {
				if len(args) > 2 {
					m.printError("unexpected argument %q", args[2])
					return m, nil
				}
				m.pushFrame(groupChecklistFrame(m.pslistGroupSelection))
				return m, nil
			}
			m.printError("unexpected argument %q", args[1])
			return m, nil
		}
		return m, m.fetchPslist(id, nil)
	default:
		m.printError("unknown command %q", args[0])
		return m, nil
	}
}

func (m Model) openPslistHistory() (tea.Model, tea.Cmd) {
	if len(m.pslistHistory) == 0 {
		m.printLine("no pslist history yet -- run `command pslist` first")
		return m, nil
	}
	m.pushFrame(pslistHistoryFrame(m.pslistHistory))
	return m, nil
}

// --- `session create` wizard ---

func (m Model) advanceWizard(line string) (tea.Model, tea.Cmd) {
	w := m.pending
	switch w.step {
	case 0:
		if profile, ok := m.moduleProfiles[line]; ok {
			m.pending = nil
			dest, err := profile.Destination()
			if err != nil {
				m.printError("%v", err)
				return m, nil
			}
			return m, m.createSession(dest)
		}
		switch strings.ToLower(line) {
		case "tcp":
			w.transport = api.Tcp
			m.printLine("host:port:")
		case "chardev":
			w.transport = api.Chardev
			m.printLine("path:")
		default:
			m.printLine("transport must be tcp, chardev, or a saved profile name; try again:")
			return m, nil
		}
		w.step = 1
		return m, nil
	case 1:
		dest, err := parseDestination(w.transport, line)
		m.pending = nil
		if err != nil {
			m.printError("%v", err)
			return m, nil
		}
		return m, m.createSession(dest)
	}
	m.pending = nil
	return m, nil
}

func parseDestination(t api.DestinationType, value string) (api.Destination, error) {
	value = strings.TrimSpace(value)
	if value == "" {
		return api.Destination{}, fmt.Errorf("enter a destination")
	}
	if t == api.Tcp {
		host, portStr, ok := strings.Cut(value, ":")
		if !ok {
			return api.Destination{}, fmt.Errorf("expected host:port")
		}
		port, err := strconv.Atoi(portStr)
		if err != nil {
			return api.Destination{}, fmt.Errorf("invalid port %q", portStr)
		}
		return api.Destination{Type: api.Tcp, Host: &host, Port: &port}, nil
	}
	return api.Destination{Type: api.Chardev, Path: &value}, nil
}
