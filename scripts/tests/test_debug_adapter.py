"""Fast DAP bridge contracts; native VS Code coverage is debugging_integration.py."""
from pathlib import Path
import shutil
import subprocess
import unittest


class DebugAdapterTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('node'), 'Node is needed for standalone JS contracts; VS Code runs the live suite')
    def test_controls_keep_console_and_toolbar_in_sync(self):
        subprocess.run(['node', str(Path(__file__).with_name('debug_adapter_test.js'))], check=True, timeout=15)
