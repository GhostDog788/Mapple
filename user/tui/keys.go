package tui

import (
	"fmt"
	"strconv"
	"strings"

	tea "charm.land/bubbletea/v2"

	"github.com/GhostDog788/Mapple/user/api"
)

func (m Model) handleKey(msg tea.KeyPressMsg) (tea.Model, tea.Cmd) {
	if msg.String() == "ctrl+c" {
		return m, tea.Quit
	}
	if len(m.frames) > 0 {
		return m.handleBrowseKey(msg)
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
	case "esc", "q":
		if m.popFrame() {
			m.input.Focus()
		}
		return m, nil
	case "enter":
		if len(top.rows) == 0 {
			return m, nil
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
		case row.push != nil:
			m.pushFrame(row.push())
		}
		return m, nil
	}
	return m, nil
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
	case "enter":
		line := strings.TrimSpace(m.input.Value())
		m.input.SetValue("")
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
	var cmd tea.Cmd
	m.input, cmd = m.input.Update(msg)
	return m, cmd
}

// --- tab completion ---

var replGrammar = map[int]map[string][]string{
	0: {"": {"session", "command", "help", "quit", "exit"}},
	1: {
		"session": {"list", "connect", "create", "delete", "help"},
		"command": {"info", "echo", "pslist", "help"},
	},
}

// replGrammar2 is keyed by "word0 word1" for the verbs that need a third
// completion level: pslist's own "history", and "help" for every "command"
// subverb (session's own subverbs take an id/no argument, so "session help"
// is already reachable at word index 1 above).
var replGrammar2 = map[string][]string{
	"command pslist": {"history", "help"},
	"command info":   {"help"},
	"command echo":   {"help"},
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
		return m, tea.Quit
	case "session":
		return m.dispatchSession(rest)
	case "command":
		return m.dispatchCommand(rest)
	default:
		m.printError("unknown command %q -- try `help`", verb)
		return m, nil
	}
}

func (m *Model) printHelp() {
	m.printLine(strings.Join([]string{
		"session          -- manage sessions -- see `session help`",
		"command          -- run a command against the connected session -- see `command help`",
		"help             -- this message",
		"quit, exit       -- exit the TUI",
		"",
		"any verb above takes a trailing `help` for more detail (`session help`,",
		"`command help`), and each command in turn has its own (`command pslist help`).",
	}, "\n"))
}

func (m *Model) printSessionHelp() {
	m.printLine(strings.Join([]string{
		"session list                    -- browse sessions in a list; enter connects, esc/q cancels",
		"session connect <id-prefix>     -- connect without opening the browser;",
		"                                   <id-prefix> just needs to be unambiguous",
		"session create                  -- wizard: pick tcp or chardev, then its",
		"                                   destination (host:port, or a device path)",
		"session delete <id-prefix>      -- delete a session by id prefix",
	}, "\n"))
}

func (m *Model) printCommandHelp() {
	m.printLine(strings.Join([]string{
		"command info                    -- session/protocol info -- see `command info help`",
		"command echo <text>             -- round-trip text through the module -- see `command echo help`",
		"command pslist                  -- process/thread snapshot -- see `command pslist help`",
		"",
		"all three require a connected session (`session connect`/`session create` first).",
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

func (m *Model) printCommandPslistHelp() {
	m.printLine(strings.Join([]string{
		"command pslist                  -- fetch a fresh process/thread snapshot",
		"                                   (kept in history, see below)",
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
		m.printLine("usage: session list|connect <id>|create|delete <id>")
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
			m.printError("unexpected argument %q", args[1])
			return m, nil
		}
		m.pending = &wizard{step: 0}
		m.printLine("transport (tcp/chardev):")
		return m, nil
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
		m.printLine("usage: command info|echo <text>|pslist [history]")
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
		case "pslist":
			m.printCommandPslistHelp()
		default:
			m.printError("unknown command %q", args[0])
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
	case "pslist":
		if len(args) > 1 {
			m.printError("unexpected argument %q", args[1])
			return m, nil
		}
		return m, m.fetchPslist(id)
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
		switch strings.ToLower(line) {
		case "tcp":
			w.transport = api.Tcp
			m.printLine("host:port:")
		case "chardev":
			w.transport = api.Chardev
			m.printLine("path:")
		default:
			m.printLine("transport must be tcp or chardev; try again:")
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
