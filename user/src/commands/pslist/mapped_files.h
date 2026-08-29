#ifndef COMMAND_PSLIST_MAPPED_FILES_H
#define COMMAND_PSLIST_MAPPED_FILES_H

#include <commands/pslist/model.h>

int mapple_pslist_build_mapped_files(struct mapple_pslist_snapshot *snapshot);
int mapple_pslist_print_json_mapped_files(const struct mapple_pslist_snapshot *snapshot, int indent);

#endif /* COMMAND_PSLIST_MAPPED_FILES_H */
