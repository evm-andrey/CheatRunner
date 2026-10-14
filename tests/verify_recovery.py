#!/usr/bin/env python3
"""Host regression checks. Requires a C compiler and pthreads, no PS5 connection."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
api = (root / 'src/cr_api.c').read_text()
handlers = []
for name in ('handle_api_health', 'handle_api_dev_shutdown'):
    start = api.index('void\n' + name + '(')
    end = api.index('\n}\n', start) + 3
    handlers.append(api[start:end])
with tempfile.TemporaryDirectory(prefix='cr-recovery-') as tmp:
    path = Path(tmp)
    (path / 'api_handlers.inc').write_text('\n'.join(handlers))
    for name, timeout in [('http_capacity', 30), ('shutdown_deadline', 20)]:
        binary = path / name
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-pthread', '-I' + tmp,
                        str(root / 'tests' / (name + '.c')), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=timeout)
        if name == "http_capacity":
            subprocess.run([str(binary), "listener"], check=True, timeout=20)
