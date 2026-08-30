package tui

import (
	"fmt"
	"strings"
	"time"

	"charm.land/bubbles/v2/textinput"
	"charm.land/bubbles/v2/viewport"
	tea "charm.land/bubbletea/v2"
	"charm.land/lipgloss/v2"

	"github.com/GhostDog788/Mapple/user/api"
)

const (
	sortByPID = iota
	sortByComm
	sortByState
)

// pslistHistoryLimit is how many past `command pslist` results the TUI keeps
// in memory (server/kernel side keep none of this -- it's purely a client
// convenience for flipping back to a recent snapshot without re-fetching).
const pslistHistoryLimit = 5

// pslistHistoryEntry is one past pslist result. Kept as a pointer in the
// history slice so re-slicing to enforce the length cap doesn't copy
// snapshots (these can be a few hundred KB for a busy machine).
type pslistHistoryEntry struct {
	snapshot  api.ProcessSnapshot
	fetchedAt time.Time
}

// taskItem is a flattened summary of one pslist task, kept only for the
// integration test's sanity checks (pid 1 is systemd, etc) -- the
// interactive browser (browse.go) reads the raw snapshot maps directly
// since it needs the Identity/Security/Files link IDs taskItem discards.
type taskItem struct {
	pid, tgid, ppid int
	state           string
	comm            string
	hasIdentity     bool
	exePath         string
}

func tasksFromSnapshot(snap api.ProcessSnapshot) []taskItem {
	identities, _ := snap.Objects["identity"].(map[string]interface{})
	flat := flattenTasks(snap.Tasks)
	items := make([]taskItem, 0, len(flat))
	for _, ft := range flat {
		raw := ft.fields
		t := taskItem{
			pid:   intField(raw, "pid"),
			tgid:  intField(raw, "tgid"),
			ppid:  intField(raw, "ppid"),
			comm:  stringField(raw, "comm"),
			state: stringField(raw, "state"),
		}
		groups, _ := raw["groups"].(map[string]interface{})
		if id, ok := groupRef(groups, "identity"); ok {
			if identity, ok := lookupByID(identities, id); ok {
				t.hasIdentity = true
				if exe, ok := identity["executable"].(map[string]interface{}); ok {
					t.exePath = stringField(exe, "path")
				}
			}
		}
		items = append(items, t)
	}
	return items
}

func intField(m map[string]interface{}, key string) int {
	if v, ok := m[key].(float64); ok {
		return int(v)
	}
	return 0
}

func stringField(m map[string]interface{}, key string) string {
	if v, ok := m[key].(string); ok {
		return v
	}
	return ""
}

func pslistSortName(mode int) string {
	switch mode {
	case sortByComm:
		return "comm"
	case sortByState:
		return "state"
	default:
		return "pid"
	}
}

// wizard tracks an in-progress multi-step `session create` prompt.
type wizard struct {
	step      int
	transport api.DestinationType
}

type Model struct {
	client         api.ClientWithResponsesInterface
	profileName    string
	profile        Profile
	moduleProfiles map[string]ModuleProfile
	configPath     string

	input    textinput.Model
	viewport viewport.Model
	lines    []string // command-line-mode transcript; browse frames render separately

	history      []string // every submitted line, oldest first -- shell-style up/down recall
	historyPos   int      // index into history; == len(history) means "not browsing, on the draft"
	historyDraft string   // input text stashed when history browsing starts, restored past the newest entry

	historySearching       bool   // ctrl+r reverse-i-search is capturing keys instead of handleReplKey
	historySearchQuery     string // typed search text; "" matches everything
	historySearchIndex     int    // position into matchingHistory's results; 0 = most recent match
	historySearchOrigInput string // input value to restore if esc cancels the search

	sessions []api.Session // last `session list` fetch, for id-prefix resolution
	current  *api.Session
	created  map[string]bool // leases this TUI created, for auto-renew

	frames    []browseFrame // navigation stack; empty = command-line mode
	pending   *wizard
	searching bool // top frame is a pslist frame and "/" is capturing a search string into input

	lastSnapshot         *api.ProcessSnapshot
	pslistSortMode       int
	pslistFilter         string
	pslistHistory        []*pslistHistoryEntry // oldest first, capped at pslistHistoryLimit
	pslistGroupSelection map[string]bool       // `command pslist groups` checklist state, keyed by pslistGroupNames

	notice       string     // pre-styled error/success toast; "" = nothing to show
	noticeExpiry time.Time  // notice is cleared once time.Now() passes this
	clientLog    []logEntry // every printError/printSuccess, oldest first, capped at clientLogLimit

	width, height int
}

// noticePopupDuration is how long an error/success toast stays visible
// before noticeTick clears it.
const noticePopupDuration = 4 * time.Second

// clientLogLimit caps how many printError/printSuccess events `log client`
// retains, same idea as pslistHistoryLimit.
const clientLogLimit = 200

// logEntry is one retained printError/printSuccess event, shown by `log
// client`. message is the plain (unstyled) formatted text.
type logEntry struct {
	at      time.Time
	level   string // "error" or "success"
	message string
}

const replPlaceholder = "session list | session create | command pslist | help"

func New(client api.ClientWithResponsesInterface, profileName string, profile Profile, moduleProfiles map[string]ModuleProfile, configPath string) Model {
	input := textinput.New()
	input.Placeholder = replPlaceholder
	input.Focus()
	vp := viewport.New()

	m := Model{
		client:               client,
		profileName:          profileName,
		profile:              profile,
		moduleProfiles:       moduleProfiles,
		configPath:           configPath,
		input:                input,
		viewport:             vp,
		created:              make(map[string]bool),
		pslistGroupSelection: defaultPslistGroupSelection(),
	}
	m.printLine("not connected -- try `session list` or `session create`. Type `help` for the full command list.")
	return m
}

func (m *Model) printLine(s string) {
	m.lines = append(m.lines, s)
	m.refreshTranscript()
}

func (m *Model) printf(format string, args ...interface{}) {
	m.printLine(fmt.Sprintf(format, args...))
}

func (m *Model) printError(format string, args ...interface{}) {
	msg := fmt.Sprintf(format, args...)
	m.showNotice(errorStyle.Render("error: " + msg))
	m.appendLog("error", msg)
}

func (m *Model) printSuccess(format string, args ...interface{}) {
	msg := fmt.Sprintf(format, args...)
	m.showNotice(successStyle.Render(msg))
	m.appendLog("success", msg)
}

// showNotice prints a pre-styled line to the transcript and also raises it
// as a toast (m.notice), so it's visible immediately even while a browse
// frame owns the viewport -- see noticeTick for how it later disappears.
func (m *Model) showNotice(styled string) {
	m.printLine(styled)
	m.notice = styled
	m.noticeExpiry = time.Now().Add(noticePopupDuration)
}

// appendLog records a printError/printSuccess event for `log client`,
// capped at clientLogLimit the same way pslistHistory is capped.
func (m *Model) appendLog(level, message string) {
	m.clientLog = append(m.clientLog, logEntry{at: time.Now(), level: level, message: message})
	if len(m.clientLog) > clientLogLimit {
		m.clientLog = m.clientLog[len(m.clientLog)-clientLogLimit:]
	}
}

func (m *Model) refreshTranscript() {
	if len(m.frames) > 0 {
		return // browse mode owns the viewport content while active
	}
	m.viewport.SetContent(strings.Join(m.lines, "\n"))
	m.viewport.GotoBottom()
}

func (m *Model) refreshBrowseView() {
	if len(m.frames) == 0 {
		m.refreshTranscript()
		return
	}
	top := m.frames[len(m.frames)-1]
	m.viewport.SetContent(strings.Join(top.render(m.width), "\n"))
	m.viewport.EnsureVisible(top.cursor+2, 0, 0) // +2 for the title/blank line render() prepends
}

// applyLayout recomputes the viewport height for the current mode: browse
// mode has no input line, so it gets one more row than command-line mode.
// Called on resize and on every push/popFrame, since switching mode doesn't
// itself produce a WindowSizeMsg.
func (m *Model) applyLayout() {
	if m.height == 0 {
		return
	}
	chrome := 6 // header + blank + status bar + input + hint + slack
	if len(m.frames) > 0 {
		chrome-- // no input line while browsing
	}
	contentHeight := m.height - chrome
	if contentHeight < 3 {
		contentHeight = 3
	}
	m.viewport.SetHeight(contentHeight)
}

// recordHistory appends a submitted line to the shell-style command
// history (skipping an immediate exact repeat, same as most shells) and
// resets history browsing back to the draft position.
func (m *Model) recordHistory(line string) {
	if len(m.history) == 0 || m.history[len(m.history)-1] != line {
		m.history = append(m.history, line)
	}
	m.historyPos = len(m.history)
	m.historyDraft = ""
}

// navigateHistory moves by delta through history (-1 = older, +1 = newer),
// stashing/restoring whatever was being typed at the draft position, same
// as a shell's up/down recall.
func (m *Model) navigateHistory(delta int) {
	if len(m.history) == 0 {
		return
	}
	if m.historyPos == len(m.history) {
		m.historyDraft = m.input.Value()
	}
	pos := m.historyPos + delta
	if pos < 0 {
		pos = 0
	}
	if pos > len(m.history) {
		pos = len(m.history)
	}
	m.historyPos = pos
	if m.historyPos == len(m.history) {
		m.input.SetValue(m.historyDraft)
	} else {
		m.input.SetValue(m.history[m.historyPos])
	}
	m.input.CursorEnd()
}

// matchingHistory returns every entry of history containing query as a
// substring, most recent first -- an empty query matches everything, so
// ctrl+r with nothing typed yet just cycles back through all of history.
func matchingHistory(history []string, query string) []string {
	var matches []string
	for i := len(history) - 1; i >= 0; i-- {
		if strings.Contains(history[i], query) {
			matches = append(matches, history[i])
		}
	}
	return matches
}

func (m *Model) statusLine() string {
	if m.current == nil {
		return "status: not connected"
	}
	s := *m.current
	return fmt.Sprintf("status: %s [%s] %s  expires %s",
		shortID(s.Id), s.State, destSummary(s.Destination), s.ExpiresAt.Format("15:04:05"))
}

func (m Model) Init() tea.Cmd {
	return tea.Batch(m.loadSessionsQuiet(), autoRenewTick(), noticeTick())
}

// noticeTick drives the toast's wall-clock expiry: a short, perpetually
// self-rescheduling tick (same pattern as autoRenewTick) rather than a
// one-shot timer returned from printError/printSuccess, since those are
// called as plain void methods from ~40 places across keys.go that would
// otherwise all need to start propagating a tea.Cmd back up to Update.
func noticeTick() tea.Cmd {
	return tea.Tick(500*time.Millisecond, func(time.Time) tea.Msg { return noticeTickMsg{} })
}

type noticeTickMsg struct{}

func (m Model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch msg := msg.(type) {
	case tea.WindowSizeMsg:
		m.width, m.height = msg.Width, msg.Height
		m.viewport.SetWidth(msg.Width)
		m.applyLayout()
		m.input.SetWidth(msg.Width)
		m.refreshBrowseView()
		return m, nil

	case tea.KeyPressMsg:
		return m.handleKey(msg)

	case sessionsLoadedMsg:
		if msg.err != nil {
			m.printError("session list: %v", msg.err)
			return m, nil
		}
		m.sessions = msg.sessions
		if msg.openBrowser {
			m.pushFrame(sessionListFrame(msg.sessions))
		}
		return m, nil

	case sessionCreatedMsg:
		if msg.err != nil {
			m.printError("session create: %v", msg.err)
			return m, nil
		}
		m.created[msg.session.Id] = true
		sess := msg.session
		m.current = &sess
		m.printSuccess("connected %s (%s)", shortID(sess.Id), destSummary(sess.Destination))
		return m, m.loadSessionsQuiet()

	case sessionDeletedMsg:
		if msg.err != nil {
			m.printError("session delete: %v", msg.err)
			return m, m.loadSessionsQuiet()
		}
		if m.current != nil && m.current.Id == msg.id {
			m.current = nil
		}
		delete(m.created, msg.id)
		m.printSuccess("deleted %s", shortID(msg.id))
		return m, m.loadSessionsQuiet()

	case sessionRenewedMsg:
		if msg.err != nil {
			m.printError("renew: %v", msg.err)
			return m, nil
		}
		if m.current != nil && m.current.Id == msg.session.Id {
			sess := msg.session
			m.current = &sess
		}
		return m, nil

	case noticeTickMsg:
		if m.notice != "" && time.Now().After(m.noticeExpiry) {
			m.notice = ""
		}
		return m, noticeTick()

	case autoRenewTickMsg:
		var cmds []tea.Cmd
		for id := range m.created {
			cmds = append(cmds, m.renewSession(id))
		}
		cmds = append(cmds, autoRenewTick())
		return m, tea.Batch(cmds...)

	case infoResultMsg:
		if msg.err != nil {
			m.printError("info: %v", msg.err)
			return m, nil
		}
		m.printSuccess("info: session_id=%d max_sessions=%d supported_commands=%s",
			msg.info.SessionId, msg.info.MaxSessions, commandNames(msg.info.SupportedCommands))
		return m, nil

	case echoResultMsg:
		if msg.err != nil {
			m.printError("echo: %v", msg.err)
			return m, nil
		}
		m.printSuccess("echo: %s", encodeEchoDisplay(msg.data))
		return m, nil

	case pslistResultMsg:
		if msg.err != nil {
			m.printError("pslist: %v", msg.err)
			return m, nil
		}
		snap := msg.snapshot
		m.lastSnapshot = &snap
		m.pslistSortMode = sortByPID
		m.pslistFilter = ""
		m.pslistHistory = append(m.pslistHistory, &pslistHistoryEntry{snapshot: snap, fetchedAt: time.Now()})
		if len(m.pslistHistory) > pslistHistoryLimit {
			m.pslistHistory = m.pslistHistory[len(m.pslistHistory)-pslistHistoryLimit:]
		}
		m.printSuccess("pslist: %d tasks, %d objects, status=%s (history %d/%d)",
			intField(snap.Snapshot, "task_count"), intField(snap.Snapshot, "object_count"), stringField(snap.Snapshot, "status"),
			len(m.pslistHistory), pslistHistoryLimit)
		m.pushFrame(pslistFrame(snap, m.pslistSortMode, m.pslistFilter))
		return m, nil
	}
	return m, nil
}

func (m *Model) connectTo(s api.Session) {
	sess := s
	m.current = &sess
	m.printSuccess("connected %s (%s)", shortID(sess.Id), destSummary(sess.Destination))
}

func (m *Model) pushFrame(f browseFrame) {
	m.frames = append(m.frames, f)
	m.applyLayout()
	m.refreshBrowseView()
}

// popFrame pops the top browse frame; returns true if it popped all the
// way out to command-line mode.
func (m *Model) popFrame() bool {
	if len(m.frames) == 0 {
		return true
	}
	m.frames = m.frames[:len(m.frames)-1]
	backToCommandLine := len(m.frames) == 0
	m.applyLayout()
	m.refreshBrowseView()
	return backToCommandLine
}

// replaceTopFrame swaps the top frame in place (same stack depth) -- used to
// re-render the pslist frame after a "/" search changes the filter, rather
// than stacking a duplicate frame the way pushFrame would.
func (m *Model) replaceTopFrame(f browseFrame) {
	if len(m.frames) == 0 {
		return
	}
	m.frames[len(m.frames)-1] = f
	m.refreshBrowseView()
}

// renderStatusBar renders the connection status as a full-width,
// reverse-video bar directly above the input line -- next to where the
// user is actually looking/typing, not scrolled away at the top with the
// rest of the transcript.
func (m Model) renderStatusBar() string {
	style := lipgloss.NewStyle().Bold(true).Reverse(true)
	if m.current != nil {
		if c := sessionStateColor(m.current.State); c != nil {
			style = style.Foreground(c)
		}
	}
	if m.width > 0 {
		style = style.Width(m.width)
	}
	return style.Render(" " + m.statusLine())
}

// viewportViewWithNotice renders the viewport, overlaying any active toast
// (m.notice) onto its last visible line instead of adding an extra line.
// Inline (non-alt-screen) rendering has no fixed scroll region, so growing
// the total output by a line just scrolls the terminal, pushing the header
// off the top instead of the toast appearing to "pop in" -- overlapping the
// bottom line keeps total height constant no matter whether a toast is up.
func (m Model) viewportViewWithNotice() string {
	content := m.viewport.View()
	if m.notice == "" {
		return content
	}
	lines := strings.Split(content, "\n")
	if len(lines) == 0 {
		return m.notice
	}
	lines[len(lines)-1] = m.notice
	return strings.Join(lines, "\n")
}

func (m Model) View() tea.View {
	var b strings.Builder
	b.WriteString(headerStyle.Render(fmt.Sprintf("mapple  profile=%s  server=%s", m.profileName, m.profile.ServerURL)))
	b.WriteString("\n\n")
	b.WriteString(m.viewportViewWithNotice())
	b.WriteString("\n")
	b.WriteString(m.renderStatusBar())
	b.WriteString("\n")
	if len(m.frames) > 0 {
		top := m.frames[len(m.frames)-1]
		if m.searching {
			b.WriteString(m.input.View() + "\n")
			b.WriteString(hintStyle.Render("enter search  esc cancel") + "\n")
		} else {
			hint := "↑/↓ move  ←/→ scroll  enter select  esc/q back  ctrl+c quit"
			if top.kind == framePslist {
				hint = "↑/↓ move  ←/→ scroll  / filter  s sort  enter select  esc/q back  ctrl+c quit"
			}
			b.WriteString(hintStyle.Render(hint) + "\n")
		}
	} else {
		b.WriteString(m.input.View() + "\n")
		b.WriteString(hintStyle.Render("tab complete  enter run  ↑/↓ history  pgup/pgdn scroll  ctrl+c quit") + "\n")
	}
	return tea.NewView(b.String())
}
