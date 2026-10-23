package server

import (
	"context"
	"errors"
	"fmt"
	"log/slog"

	"github.com/GhostDog788/Mapple/user/api"
	"github.com/GhostDog788/Mapple/user/server/internal/moduleclient"
	"github.com/GhostDog788/Mapple/user/server/internal/moduleclient/wire"
)

type Server struct {
	Sessions *SessionManager
	Logger   *slog.Logger
	cfg      *Config
}

func New(cfg *Config, sessions *SessionManager, logger *slog.Logger) *Server {
	return &Server{Sessions: sessions, Logger: logger, cfg: cfg}
}

func apiDestination(d Destination) api.Destination {
	out := api.Destination{Type: api.DestinationType(d.Type)}
	if d.Type == DestinationTCP {
		out.Host = &d.Host
		port := d.Port
		out.Port = &port
	} else {
		out.Path = &d.Path
	}
	return out
}

func destinationFromAPI(d api.Destination) Destination {
	out := Destination{Type: DestinationType(d.Type)}
	if d.Host != nil {
		out.Host = *d.Host
	}
	if d.Port != nil {
		out.Port = *d.Port
	}
	if d.Path != nil {
		out.Path = *d.Path
	}
	return out
}

func apiInfoResult(info moduleclient.InfoResult) api.InfoResult {
	return api.InfoResult{
		SessionId:         int64(info.SessionID),
		MaxSessions:       int(info.MaxSessions),
		SupportedCommands: commandNames(info.SupportedCommands),
	}
}

// commandNameByBit mirrors the wire protocol's own Command bit positions
// (moduleclient/wire.Command* -- CommandEcho=1, CommandPslist=2,
// CommandInfo=3, CommandGetfile=4), each a bit index into the info response's
// supported_commands mask.
var commandNameByBit = map[uint]string{
	uint(wire.CommandEcho):    "echo",
	uint(wire.CommandPslist):  "pslist",
	uint(wire.CommandInfo):    "info",
	uint(wire.CommandGetfile): "getfile",
}

// commandNames renders a supported_commands bitmask as the actual command
// names it advertises rather than raw bits -- any bit this build doesn't
// recognize (a newer module) is kept, not silently dropped, as "bit_N".
func commandNames(mask uint64) []string {
	names := []string{}
	for bit := uint(0); bit < 64; bit++ {
		if mask&(1<<bit) == 0 {
			continue
		}
		if name, ok := commandNameByBit[bit]; ok {
			names = append(names, name)
		} else {
			names = append(names, fmt.Sprintf("bit_%d", bit))
		}
	}
	return names
}

func apiSession(l *Lease) api.Session {
	info := apiInfoResult(l.Client.InitialInfo())
	return api.Session{
		Id:          l.ID,
		State:       api.SessionState(l.State()),
		Destination: apiDestination(l.Destination),
		Info:        &info,
		CreatedAt:   l.CreatedAt,
		ExpiresAt:   l.ExpiresAt(),
	}
}

func apiError(code, message string) api.Error {
	return api.Error{Code: code, Message: message}
}

// --- health / status ---

func (s *Server) GetHealthLive(ctx context.Context, request api.GetHealthLiveRequestObject) (api.GetHealthLiveResponseObject, error) {
	return api.GetHealthLive200JSONResponse{Status: api.Ok}, nil
}

func (s *Server) GetStatus(ctx context.Context, request api.GetStatusRequestObject) (api.GetStatusResponseObject, error) {
	return api.GetStatus200JSONResponse{
		ActiveLeases: s.Sessions.ActiveCount(),
		MaxSessions:  0, // no daemon-wide cap; the kernel/OS are the only capacity authorities
	}, nil
}

// --- sessions ---

func (s *Server) GetSessions(ctx context.Context, request api.GetSessionsRequestObject) (api.GetSessionsResponseObject, error) {
	leases := s.Sessions.List()
	out := make(api.GetSessions200JSONResponse, 0, len(leases))
	for _, lease := range leases {
		out = append(out, apiSession(lease))
	}
	return out, nil
}

func (s *Server) PostSessions(ctx context.Context, request api.PostSessionsRequestObject) (api.PostSessionsResponseObject, error) {
	if request.Body == nil {
		return api.PostSessions400JSONResponse{BadRequestJSONResponse: api.BadRequestJSONResponse(apiError("invalid_argument", "request body is required"))}, nil
	}
	dest := destinationFromAPI(*request.Body)
	lease, err := s.Sessions.Create(ctx, dest)
	if err != nil {
		if errors.Is(err, context.DeadlineExceeded) {
			return api.PostSessions504JSONResponse(apiError("handshake_timeout", err.Error())), nil
		}
		return api.PostSessions400JSONResponse{BadRequestJSONResponse: api.BadRequestJSONResponse(apiError("invalid_argument", err.Error()))}, nil
	}
	return api.PostSessions201JSONResponse(apiSession(lease)), nil
}

func (s *Server) GetSessionsId(ctx context.Context, request api.GetSessionsIdRequestObject) (api.GetSessionsIdResponseObject, error) {
	lease, ok := s.Sessions.Get(request.Id)
	if !ok {
		return api.GetSessionsId404JSONResponse{NotFoundJSONResponse: api.NotFoundJSONResponse(apiError("not_found", "no such lease"))}, nil
	}
	return api.GetSessionsId200JSONResponse(apiSession(lease)), nil
}

func (s *Server) DeleteSessionsId(ctx context.Context, request api.DeleteSessionsIdRequestObject) (api.DeleteSessionsIdResponseObject, error) {
	s.Sessions.Delete(request.Id) // idempotent: no-op if already absent
	return api.DeleteSessionsId204Response{}, nil
}

func (s *Server) PostSessionsIdRenew(ctx context.Context, request api.PostSessionsIdRenewRequestObject) (api.PostSessionsIdRenewResponseObject, error) {
	lease, ok := s.Sessions.Renew(request.Id)
	if !ok {
		return api.PostSessionsIdRenew404JSONResponse{NotFoundJSONResponse: api.NotFoundJSONResponse(apiError("not_found", "no such lease"))}, nil
	}
	return api.PostSessionsIdRenew200JSONResponse(apiSession(lease)), nil
}

// --- commands ---

func (s *Server) leaseFor(id string) (*Lease, api.Error, int) {
	lease, ok := s.Sessions.Get(id)
	if !ok {
		return nil, apiError("not_found", "no such lease"), 404
	}
	if lease.Client.Broken() {
		return nil, apiError("session_broken", "lease's session is broken; delete and recreate it"), 410
	}
	return lease, api.Error{}, 0
}

func (s *Server) PostSessionsIdCommandsInfo(ctx context.Context, request api.PostSessionsIdCommandsInfoRequestObject) (api.PostSessionsIdCommandsInfoResponseObject, error) {
	lease, apiErr, status := s.leaseFor(request.Id)
	if lease == nil {
		return commandError[api.PostSessionsIdCommandsInfoResponseObject](status, apiErr,
			func(e api.NotFoundJSONResponse) api.PostSessionsIdCommandsInfoResponseObject {
				return api.PostSessionsIdCommandsInfo404JSONResponse{NotFoundJSONResponse: e}
			},
			func(e api.BrokenJSONResponse) api.PostSessionsIdCommandsInfoResponseObject {
				return api.PostSessionsIdCommandsInfo410JSONResponse{BrokenJSONResponse: e}
			}), nil
	}
	info, err := lease.Client.Info(ctx)
	if err != nil {
		if errors.Is(err, moduleclient.ErrSessionBusy) {
			return api.PostSessionsIdCommandsInfo409JSONResponse{BusyJSONResponse: api.BusyJSONResponse(apiError("session_busy", err.Error()))}, nil
		}
		return api.PostSessionsIdCommandsInfo410JSONResponse{BrokenJSONResponse: api.BrokenJSONResponse(apiError("session_broken", err.Error()))}, nil
	}
	s.Sessions.Renew(lease.ID)
	return api.PostSessionsIdCommandsInfo200JSONResponse(apiInfoResult(info)), nil
}

func (s *Server) PostSessionsIdCommandsEcho(ctx context.Context, request api.PostSessionsIdCommandsEchoRequestObject) (api.PostSessionsIdCommandsEchoResponseObject, error) {
	lease, apiErr, status := s.leaseFor(request.Id)
	if lease == nil {
		return commandError[api.PostSessionsIdCommandsEchoResponseObject](status, apiErr,
			func(e api.NotFoundJSONResponse) api.PostSessionsIdCommandsEchoResponseObject {
				return api.PostSessionsIdCommandsEcho404JSONResponse{NotFoundJSONResponse: e}
			},
			func(e api.BrokenJSONResponse) api.PostSessionsIdCommandsEchoResponseObject {
				return api.PostSessionsIdCommandsEcho410JSONResponse{BrokenJSONResponse: e}
			}), nil
	}
	if request.Body == nil {
		return nil, errors.New("request body is required")
	}
	echoed, err := lease.Client.Echo(ctx, request.Body.Data)
	if err != nil {
		if errors.Is(err, moduleclient.ErrSessionBusy) {
			return api.PostSessionsIdCommandsEcho409JSONResponse{BusyJSONResponse: api.BusyJSONResponse(apiError("session_busy", err.Error()))}, nil
		}
		return api.PostSessionsIdCommandsEcho410JSONResponse{BrokenJSONResponse: api.BrokenJSONResponse(apiError("session_broken", err.Error()))}, nil
	}
	s.Sessions.Renew(lease.ID)
	return api.PostSessionsIdCommandsEcho200JSONResponse{Data: echoed}, nil
}

func (s *Server) PostSessionsIdCommandsGetfile(ctx context.Context, request api.PostSessionsIdCommandsGetfileRequestObject) (api.PostSessionsIdCommandsGetfileResponseObject, error) {
	lease, apiErr, status := s.leaseFor(request.Id)
	if lease == nil {
		return commandError[api.PostSessionsIdCommandsGetfileResponseObject](status, apiErr,
			func(e api.NotFoundJSONResponse) api.PostSessionsIdCommandsGetfileResponseObject {
				return api.PostSessionsIdCommandsGetfile404JSONResponse{NotFoundJSONResponse: e}
			},
			func(e api.BrokenJSONResponse) api.PostSessionsIdCommandsGetfileResponseObject {
				return api.PostSessionsIdCommandsGetfile410JSONResponse{BrokenJSONResponse: e}
			}), nil
	}
	if request.Body == nil {
		return api.PostSessionsIdCommandsGetfile400JSONResponse{BadRequestJSONResponse: api.BadRequestJSONResponse(apiError("invalid_argument", "request body with path is required"))}, nil
	}
	data, err := lease.Client.Getfile(ctx, request.Body.Path)
	if err != nil {
		if errors.Is(err, moduleclient.ErrInvalidGetfilePath) {
			return api.PostSessionsIdCommandsGetfile400JSONResponse{BadRequestJSONResponse: api.BadRequestJSONResponse(apiError("invalid_argument", err.Error()))}, nil
		}
		var rejection *moduleclient.CommandError
		if errors.As(err, &rejection) {
			s.Sessions.Renew(lease.ID)
			if rejection.Status == wire.StatusInvalidArgument {
				return api.PostSessionsIdCommandsGetfile400JSONResponse{BadRequestJSONResponse: api.BadRequestJSONResponse(apiError("invalid_argument", "module rejected the path: "+moduleclient.ErrInvalidGetfilePath.Error()))}, nil
			}
			return api.PostSessionsIdCommandsGetfile422JSONResponse(apiError("module_command_rejected", getfileStatusMessage(rejection.Status))), nil
		}
		if errors.Is(err, moduleclient.ErrSessionBusy) {
			return api.PostSessionsIdCommandsGetfile409JSONResponse{BusyJSONResponse: api.BusyJSONResponse(apiError("session_busy", err.Error()))}, nil
		}
		return api.PostSessionsIdCommandsGetfile410JSONResponse{BrokenJSONResponse: api.BrokenJSONResponse(apiError("session_broken", err.Error()))}, nil
	}
	s.Sessions.Renew(lease.ID)
	return &getfileResponse{file: data, sessions: s.Sessions, leaseID: lease.ID, logger: s.Logger}, nil
}

func (s *Server) PostSessionsIdCommandsPslist(ctx context.Context, request api.PostSessionsIdCommandsPslistRequestObject) (api.PostSessionsIdCommandsPslistResponseObject, error) {
	lease, apiErr, status := s.leaseFor(request.Id)
	if lease == nil {
		return commandError[api.PostSessionsIdCommandsPslistResponseObject](status, apiErr,
			func(e api.NotFoundJSONResponse) api.PostSessionsIdCommandsPslistResponseObject {
				return api.PostSessionsIdCommandsPslist404JSONResponse{NotFoundJSONResponse: e}
			},
			func(e api.BrokenJSONResponse) api.PostSessionsIdCommandsPslistResponseObject {
				return api.PostSessionsIdCommandsPslist410JSONResponse{BrokenJSONResponse: e}
			}), nil
	}
	var groups uint64
	if request.Body != nil && request.Body.RequestedGroups != nil {
		groups = groupsFromAPI(*request.Body.RequestedGroups)
	}
	snapshot, err := lease.Client.ProcessSnapshot(ctx, groups)
	if err != nil {
		var unsupported moduleclient.ErrPslistGroupsUnsupported
		if errors.As(err, &unsupported) {
			return api.PostSessionsIdCommandsPslist400JSONResponse(apiError("invalid_argument", err.Error())), nil
		}
		if errors.Is(err, moduleclient.ErrSessionBusy) {
			return api.PostSessionsIdCommandsPslist409JSONResponse{BusyJSONResponse: api.BusyJSONResponse(apiError("session_busy", err.Error()))}, nil
		}
		return api.PostSessionsIdCommandsPslist410JSONResponse{BrokenJSONResponse: api.BrokenJSONResponse(apiError("session_broken", err.Error()))}, nil
	}
	s.Sessions.Renew(lease.ID)
	resp, err := apiProcessSnapshot(snapshot)
	if err != nil {
		return nil, err
	}
	return api.PostSessionsIdCommandsPslist200JSONResponse(resp), nil
}

// commandError adapts a (code, apiErr) pair from leaseFor into the right
// generated *_404/*_410 response type for a given operation, without
// duplicating the not-found/broken branching in every command handler.
func commandError[R any](status int, apiErr api.Error, notFound func(api.NotFoundJSONResponse) R, broken func(api.BrokenJSONResponse) R) R {
	if status == 404 {
		return notFound(api.NotFoundJSONResponse(apiErr))
	}
	return broken(api.BrokenJSONResponse(apiErr))
}

func groupsFromAPI(list []api.PslistGroup) uint64 {
	var bits uint64
	for _, g := range list {
		switch g {
		case api.Identity:
			bits |= moduleclient.GroupIdentity
		case api.Environment:
			bits |= moduleclient.GroupEnvironment
		case api.Security:
			bits |= moduleclient.GroupSecurity
		case api.Context:
			bits |= moduleclient.GroupContext
		case api.Resources:
			bits |= moduleclient.GroupResources
		case api.Fds:
			bits |= moduleclient.GroupFDs
		case api.FdsDetails:
			bits |= moduleclient.GroupFDsDetails
		case api.Network:
			bits |= moduleclient.GroupNetwork
		case api.MemoryMaps:
			bits |= moduleclient.GroupMemoryMaps
		case api.Elf:
			bits |= moduleclient.GroupELF
		}
	}
	return bits
}

func groupsToAPI(bits uint64) []api.PslistGroup {
	var out []api.PslistGroup
	if bits&moduleclient.GroupIdentity != 0 {
		out = append(out, api.Identity)
	}
	if bits&moduleclient.GroupEnvironment != 0 {
		out = append(out, api.Environment)
	}
	if bits&moduleclient.GroupSecurity != 0 {
		out = append(out, api.Security)
	}
	if bits&moduleclient.GroupContext != 0 {
		out = append(out, api.Context)
	}
	if bits&moduleclient.GroupResources != 0 {
		out = append(out, api.Resources)
	}
	if bits&moduleclient.GroupFDs != 0 {
		out = append(out, api.Fds)
	}
	if bits&moduleclient.GroupFDsDetails != 0 {
		out = append(out, api.FdsDetails)
	}
	if bits&moduleclient.GroupNetwork != 0 {
		out = append(out, api.Network)
	}
	if bits&moduleclient.GroupMemoryMaps != 0 {
		out = append(out, api.MemoryMaps)
	}
	if bits&moduleclient.GroupELF != 0 {
		out = append(out, api.Elf)
	}
	return out
}

// apiProcessSnapshot (pslistview.go) builds the response from the
// already-decoded view functions -- this file only wires it into the
// pslist command handler above.
