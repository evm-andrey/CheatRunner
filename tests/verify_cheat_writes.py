#!/usr/bin/env python3
"""Compile verbatim write/rollback code with injected host memory I/O failures."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'src/cr_cheats.c').read_text()
start = source.index('    for (int pass = 0; pass < 2 && rc == 0; pass++)')
end = source.index('  /* ---- Rollback on failure ---- */', start)
loop = source[start:end].rstrip()
assert loop.endswith('  }')
loop = loop[:-3]  # omit the enclosing preflight-success block's closing brace
start = source.index('  if (rc != 0 && write_attempt_n > 0)', end)
end = source.index(' else if (rc == 0)', start)
with tempfile.TemporaryDirectory(prefix='cr-write-test-') as tmp:
    path = Path(tmp)
    (path / 'cheat_write_loop.inc').write_text(loop + source[start:end])
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-I' + tmp,
                    str(root / 'tests/cheat_writes.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=5)
