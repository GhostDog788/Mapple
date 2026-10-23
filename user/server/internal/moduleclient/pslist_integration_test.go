//go:build integration

package moduleclient

import (
	"context"
	"fmt"
	"testing"
	"time"

	"github.com/GhostDog788/Mapple/user/server/internal/moduleclient/wire"
)

func TestIntegration_ProcessSnapshotGroups(t *testing.T) {
	host, port := testTCPTarget(t)
	client, err := NewTCP(context.Background(), host, port)
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	if err := client.t.SetDeadline(time.Now().Add(60 * time.Second)); err != nil {
		t.Fatal(err)
	}

	groups := []uint64{0, GroupIdentity, GroupEnvironment, GroupSecurity, GroupContext,
		GroupResources, GroupFDs, GroupFDsDetails, GroupNetwork, GroupMemoryMaps, GroupELF,
		GroupFDsDetails | GroupNetwork, DefaultGroups}
	for _, requested := range groups {
		t.Run(fmt.Sprintf("groups_%x", requested), func(t *testing.T) {
			snapshot, err := client.processSnapshot(requested)
			if err != nil {
				t.Fatal(err)
			}
			effective := requested
			if requested&(GroupFDsDetails|GroupNetwork) != 0 {
				effective |= GroupFDs
			}
			if requested&GroupELF != 0 {
				effective |= GroupMemoryMaps
			}
			if snapshot.RequestedGroups != requested || snapshot.EffectiveGroups != effective {
				t.Fatalf("requested=%#x effective=%#x, want %#x/%#x",
					snapshot.RequestedGroups, snapshot.EffectiveGroups, requested, effective)
			}
			if len(snapshot.Tasks) == 0 || int(snapshot.TaskCount) != len(snapshot.Tasks) {
				t.Fatalf("task_count=%d, decoded=%d", snapshot.TaskCount, len(snapshot.Tasks))
			}
			for _, task := range snapshot.Tasks {
				for group, id := range map[uint64]*uint64{
					GroupIdentity: task.IdentityID, GroupEnvironment: task.EnvironmentID,
					GroupSecurity: task.SecurityID, GroupContext: task.ContextID,
					GroupResources: task.ResourcesID, GroupFDs: task.FilesID,
					GroupNetwork: task.NetworkID, GroupMemoryMaps: task.MemoryMapsID,
				} {
					if id != nil && (effective&group == 0 || *id == 0) {
						t.Fatalf("pid %d: unexpected link for group %#x", task.PID, group)
					}
				}
				if task.FilesID == nil {
					continue
				}
				files := snapshot.FilesObjects[*task.FilesID]
				if files == nil {
					continue // The existing decoder omits empty FD objects.
				}
				for _, fd := range files.FDs {
					if (fd.Details != nil) != (effective&GroupFDsDetails != 0) {
						t.Fatalf("fd %d: unexpected details presence", fd.FD.FD)
					}
					if fd.Details != nil && (fd.Details.ObjectID != *task.FilesID || fd.Details.FD != fd.FD.FD) {
						t.Fatalf("fd %d: details belong to another FD", fd.FD.FD)
					}
					expectNetwork := effective&GroupNetwork != 0 && fd.Kind == wire.FDSocket
					if (fd.Network != nil) != expectNetwork {
						t.Fatalf("fd %d: unexpected network presence", fd.FD.FD)
					}
					if fd.Network != nil && (task.NetworkID == nil || fd.Network.ObjectID != *task.NetworkID || fd.Network.FD != fd.FD.FD) {
						t.Fatalf("fd %d: network record belongs to another FD", fd.FD.FD)
					}
				}
			}
			for _, maps := range snapshot.MemoryMaps {
				for _, mapping := range maps.Mappings {
					if mapping.ELFObjectID != 0 && (effective&GroupELF == 0 || snapshot.ELFs[mapping.ELFObjectID] == nil) {
						t.Fatalf("mapping references missing ELF object %d", mapping.ELFObjectID)
					}
				}
			}
			got, err := client.Echo(context.Background(), []byte("after-pslist"))
			if err != nil || string(got) != "after-pslist" {
				t.Fatalf("session after snapshot: echo=%q err=%v", got, err)
			}
		})
	}
}
