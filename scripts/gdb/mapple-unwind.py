"""Supply read-only Linux x86-64 exception CFI to GDB (including MI/VS Code).

Linux's assembly IDT entries have ORC hints, not DWARF frame information. After
verifying the ordinary entry sequence and the matching kernel's pt_regs layout,
provide a local, symbol-only ELF containing the missing DWARF signal-frame CFI.
GDB's native unwinder then restores the saved registers in its *view*, including
the exact fault PC. Python unwinders cannot mark a frame as a signal frame on
GDB 15; treating it as a call would incorrectly use PC-1 for source/locals.

Only debugger metadata and temporary host files are written. In particular,
add-symbol-file does NOT load code into the target. No guest registers/memory
are assigned and no inferior functions or execution commands are invoked.
"""
import os
import struct
import tempfile

import gdb


# Ordinary idtentry_body entries in arch/x86/entry/entry_64.S. Deliberately
# exclude IST/paranoid/NMI/double-fault entries and the int3 stack-gap path.
HANDLERS = (
    'exc_divide_error', 'exc_overflow', 'exc_bounds', 'exc_invalid_op',
    'exc_device_not_available', 'exc_coproc_segment_overrun', 'exc_invalid_tss',
    'exc_segment_not_present', 'exc_stack_segment', 'exc_general_protection',
    'exc_page_fault', 'exc_spurious_interrupt_bug', 'exc_coprocessor_error',
    'exc_alignment_check', 'exc_simd_coprocessor_error',
    'exc_control_protection', 'exc_virtualization_exception',
)
FIELDS = ('r15', 'r14', 'r13', 'r12', 'bp', 'bx', 'r11', 'r10', 'r9',
          'r8', 'ax', 'cx', 'dx', 'si', 'di', 'orig_ax', 'ip', 'cs',
          'flags', 'sp', 'ss')
# AMD64 DWARF register numbers, not GDB's architecture-dependent numbering.
REGISTERS = {
    'ax': 0, 'dx': 1, 'cx': 2, 'bx': 3, 'si': 4, 'di': 5, 'bp': 6, 'sp': 7,
    'r8': 8, 'r9': 9, 'r10': 10, 'r11': 11, 'r12': 12, 'r13': 13,
    'r14': 14, 'r15': 15, 'ip': 16, 'flags': 49, 'cs': 51, 'ss': 52,
}


def uleb(value):
    result = bytearray()
    while value >= 128:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return result


def record(data):
    data += bytes((-len(data)) % 8)  # DW_CFA_nop padding
    return struct.pack('<I', len(data)) + data


def frame_elf(points):
    """Minimal x86-64 ELF: NOBITS text and absolute .debug_frame entries.

    There is no target code or data to load. CFI applies only at the return
    address of each verified stub's C call (and PC-1 used to find that frame).
    """
    size = len(FIELDS) * 8
    # CIE v3, augmentation S = signal frame, code alignment 1, data alignment
    # -8 (SLEB 0x78), return register RIP. CFA = current RSP + sizeof(pt_regs).
    cie = bytearray(b'\xff\xff\xff\xff\x03S\x00\x01\x78\x10')
    cie += b'\x0c\x07' + uleb(size)  # DW_CFA_def_cfa
    for field, register in REGISTERS.items():
        cie += bytes([0x80 | register]) + uleb((size - FIELDS.index(field) * 8) // 8)
    frames = bytearray(record(cie))
    for pc in sorted(set(points)):
        frames += record(struct.pack('<IQQ', 0, pc - 1, 2))
    names = b'\0.text\0.debug_frame\0.shstrtab\0'
    content = bytearray(64)
    frame_offset = len(content)
    content += frames
    names_offset = len(content)
    content += names
    content += bytes((-len(content)) % 8)
    section_offset = len(content)
    first, last = min(points) - 1, max(points) + 1
    sections = (
        (0,) * 10,
        (1, 8, 6, first, 0, last - first, 0, 0, 1, 0),  # .text NOBITS
        (7, 1, 0, 0, frame_offset, len(frames), 0, 0, 8, 0),
        (20, 3, 0, 0, names_offset, len(names), 0, 0, 1, 0),
    )
    for section in sections:
        content += struct.pack('<IIQQQQIIQQ', *section)
    content[:64] = (b'\x7fELF\x02\x01\x01' + bytes(9) + struct.pack(
        '<HHIQQQIHHHHHH', 2, 62, 1, 0, 0, section_offset, 0,
        64, 0, 0, 64, len(sections), 3))
    return content


def entry_return(arch, inferior, handler):
    """Accept only mov RSP,RDI; [error-code setup]; direct call C-handler."""
    start = int(gdb.parse_and_eval('&asm_' + handler))
    target = int(gdb.parse_and_eval('&' + handler))
    pc, ready = start, False
    while pc < start + 128:
        instruction = arch.disassemble(pc, count=1)[0]
        data = bytes(inferior.read_memory(pc, instruction['length']))
        end = pc + len(data)
        if data == b'\x48\x89\xe7':  # mov %rsp,%rdi
            ready = True
        elif ready:
            if len(data) == 5 and data[0] == 0xe8:
                destination = end + int.from_bytes(data[1:], 'little', signed=True)
                return end if destination == target else None
            if data not in (b'\x48\x8b\x74\x24\x78',  # mov ORIG_RAX(%rsp),%rsi
                            b'\x48\xc7\x44\x24\x78\xff\xff\xff\xff'):
                return None
        # Do not walk into adjacent functions if this isn't an ordinary entry.
        if data[0] in (0xc3, 0xe9, 0xeb) or data == b'\x0f\x0b':
            return None
        pc = end
    return None


def install():
    inferior = gdb.selected_inferior()
    arch = inferior.architecture()
    if arch.name() != 'i386:x86-64':
        gdb.write('Mapple exception unwinding: only Linux x86-64 is supported.\n')
        return
    layout = gdb.lookup_type('struct pt_regs')
    offsets = {f.name: f.bitpos for f in layout.fields()}
    if layout.sizeof != 168 or any(offsets.get(f) != i * 64 for i, f in enumerate(FIELDS)):
        gdb.write('Mapple exception unwinding: unsupported pt_regs layout; left unchanged.\n')
        return
    points = []
    for handler in HANDLERS:
        try:
            pc = entry_return(arch, inferior, handler)
            if pc is not None:
                points.append(pc)
        except gdb.error:
            # Missing symbols, inaccessible memory or an unsupported entry must
            # never manufacture a frame. Other supported entries still work.
            continue
    if not points:
        gdb.write('Mapple exception unwinding: no supported entry sequences found.\n')
        return
    # Keep files alive as long as their symbol objfile is live. Re-sourcing is
    # safe, including after a kernel symbol/target change in the same GDB.
    state = getattr(gdb, '_mapple_exception_cfi', {})
    space = gdb.current_progspace()
    old = state.get(space)
    if old and old[0].is_valid():
        gdb.execute('remove-symbol-file "' + old[1].name + '/exceptions.elf"', to_string=True)
    if old:
        old[1].cleanup()
    temporary = tempfile.TemporaryDirectory(prefix='mapple-gdb-')
    filename = os.path.join(temporary.name, 'exceptions.elf')
    try:
        with open(filename, 'wb') as output:
            output.write(frame_elf(points))
        gdb.execute('add-symbol-file "' + filename + '"', to_string=True)
        objfile = next(obj for obj in space.objfiles() if obj.filename == filename)
    except Exception:
        temporary.cleanup()
        raise
    state[space] = (objfile, temporary)
    gdb._mapple_exception_cfi = state
    gdb.invalidate_cached_frames()
    gdb.write('Mapple exception unwinding: enabled for %d x86-64 entries (read-only).\n' % len(points))


try:
    install()
except gdb.error as error:
    gdb.write('Mapple exception unwinding unavailable: %s\n' % error)
