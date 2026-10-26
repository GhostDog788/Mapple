"""Exercise real GDB unwinding using an unprivileged Linux-entry fixture."""
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / 'scripts/tests/fixtures/exception.c'
HELPER = ROOT / 'scripts/gdb/mapple-unwind.py'


def build_fixture(directory, extra=()):
    binary = directory / 'exception'
    subprocess.run(['gcc', '-O2', '-g', '-fomit-frame-pointer', '-mno-red-zone',
                    '-no-pie', *extra, str(FIXTURE), str(FIXTURE.with_suffix('.S')),
                    '-o', str(binary)], check=True, capture_output=True)
    return binary


@unittest.skipUnless(platform.machine() == 'x86_64' and shutil.which('gcc') and shutil.which('gdb'),
                     'x86-64 GCC and Python-enabled GDB required')
class ExceptionUnwindTests(unittest.TestCase):
    def test_fault_frames_locals_and_registers_are_read_only_at_each_stop(self):
        cases = [(stop, ()) for stop in ('exc_divide_error', 'die', 'panic')]
        cases.append(('die', ('-DTEST_ERROR_CODE', '-Dexc_divide_error=exc_page_fault',
                              '-Dasm_exc_divide_error=asm_exc_page_fault')))
        for stop, extra in cases:
            with self.subTest(stop=stop, extra=extra), tempfile.TemporaryDirectory(prefix='mapple-unwind-') as tmp:
                directory = Path(tmp)
                binary = build_fixture(directory, extra)
                checks = directory / 'checks.py'
                checks.write_text('''import gdb
def frames():
    frame = gdb.newest_frame()
    result = []
    while frame:
        result.append(frame)
        frame = frame.older()
    return result
def snapshot():
    top = gdb.newest_frame()
    registers = {name: str(top.read_register(name)) for name in
                 ('rax','rbx','rcx','rdx','rsi','rdi','rbp','rsp','rip',
                  'r8','r9','r10','r11','r12','r13','r14','r15','eflags')}
    stack = int(top.read_register('rsp'))
    saved = int(gdb.parse_and_eval('captured->sp'))
    memory = bytes(gdb.selected_inferior().read_memory(stack, saved - stack + 128))
    return registers, memory
before = snapshot()
assert 'divide_by_zero' not in [f.name() for f in frames()]
for setting in ('may-write-registers', 'may-write-memory', 'may-insert-breakpoints', 'may-call-functions'):
    gdb.execute('set ' + setting + ' off')
''' + f'helper = {str(HELPER)!r}\n' + '''for repetition in range(2):
    gdb.execute('source ' + helper)
    stack = frames()
    names = [f.name() for f in stack]
    assert 'divide_by_zero' in names and 'command' in names, names
    fault = next(f for f in stack if f.name() == 'divide_by_zero')
    caller = next(f for f in stack if f.name() == 'command')
    assert fault.newer().type() == gdb.SIGTRAMP_FRAME
    saved = gdb.parse_and_eval('*captured')
    assert fault.pc() == int(saved['ip'])
    assert fault.find_sal().line == gdb.find_pc_line(int(saved['ip'])).line
    assert int(fault.read_var('divisor')) == 0
    assert int(fault.read_var('marker')) == 1234
    assert int(fault.read_var('numerator')) == 5
    assert int(caller.read_var('request_id')) == 42
    assert caller.read_var('path').string() == '/fixture/file'
    mapping = {'rax':'ax','rbx':'bx','rcx':'cx','rdx':'dx','rdi':'di','rsi':'si',
               'rbp':'bp','rsp':'sp','rip':'ip','eflags':'flags','cs':'cs','ss':'ss'}
    mapping.update({name:name for name in ('r8','r9','r10','r11','r12','r13','r14','r15')})
    for register, field in mapping.items():
        assert int(fault.read_register(register)) == int(saved[field]), register
    fault.select()
    assert int(gdb.parse_and_eval('divisor')) == 0
    gdb.newest_frame().select()
    assert snapshot() == before, 'unwinding changed inferior state'
print('EXCEPTION_UNWIND_PASS')
''')
                commands = directory / 'test.gdb'
                commands.write_text('set pagination off\nset confirm off\nset debuginfod enabled off\n'
                                    'handle SIGFPE nostop noprint pass\n'
                                    f'break {stop}\nrun\ngenerate-core-file {directory / "exception.core"}\n'
                                    f'kill\ncore-file {directory / "exception.core"}\nsource {checks}\n')
                result = subprocess.run(['gdb', '-nx', '-q', '-batch', str(binary), '-x', str(commands)],
                                        capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('EXCEPTION_UNWIND_PASS', result.stdout, result.stdout + result.stderr)
                self.assertNotIn('Traceback', result.stderr)

    def test_unknown_entry_instructions_do_not_install_cfi(self):
        with tempfile.TemporaryDirectory(prefix='mapple-unwind-') as tmp:
            binary = build_fixture(Path(tmp), ('-DTEST_UNSUPPORTED_ENTRY',))
            result = subprocess.run(['gdb', '-nx', '-q', '-batch', str(binary),
                                     '-ex', 'source ' + str(HELPER), '-ex', 'info files'],
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('no supported entry sequences', result.stdout)
            self.assertNotIn('exceptions.elf', result.stdout)

    def test_unknown_layout_and_missing_symbols_do_not_install_cfi(self):
        with tempfile.TemporaryDirectory(prefix='mapple-unwind-') as tmp:
            directory = Path(tmp)
            source = directory / 'unsupported.c'
            source.write_text('struct pt_regs { int wrong; }; struct pt_regs regs; int main(void) { return 0; }')
            binary = directory / 'unsupported'
            for debug in (True, False):
                subprocess.run(['gcc', *(['-g'] if debug else []), str(source), '-o', str(binary)], check=True)
                result = subprocess.run(['gdb', '-nx', '-q', '-batch', str(binary),
                                         '-ex', 'source ' + str(HELPER), '-ex', 'info files'],
                                        capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertNotIn('exceptions.elf', result.stdout)
                self.assertIn('unsupported pt_regs' if debug else 'unavailable', result.stdout)


if __name__ == '__main__':
    unittest.main()
