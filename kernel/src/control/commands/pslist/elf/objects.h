#ifndef MAPPLE_COMMAND_PSLIST_ELF_OBJECTS_H
#define MAPPLE_COMMAND_PSLIST_ELF_OBJECTS_H

#include <linux/fs.h>
#include <linux/hashtable.h>
#include <linux/list.h>

#include <control/commands/pslist/snapshot/stream.h>

#define MAPPLE_PSLIST_ELF_HASH_BITS 8

struct mapple_pslist_elf_registry {
	DECLARE_HASHTABLE(files, MAPPLE_PSLIST_ELF_HASH_BITS);
	struct list_head order;
	u64 next_object_id;
};

void mapple_pslist_elf_registry_init(struct mapple_pslist_elf_registry *registry);
void mapple_pslist_elf_registry_release(struct mapple_pslist_elf_registry *registry);
int mapple_pslist_elf_observe_file(struct mapple_pslist_elf_registry *registry, struct file *file,
				   u64 *object_id);
int mapple_pslist_write_elf_objects(struct mapple_pslist_writer *writer,
				    struct mapple_pslist_elf_registry *registry);

#endif /* MAPPLE_COMMAND_PSLIST_ELF_OBJECTS_H */
