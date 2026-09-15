"""Verify --wrap reached the actual SDK dispatch table in a linked MCU ELF."""
import pathlib
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[1]
objdump = root / 'tools/toolchain/riscv-none-embed-gcc8/bin/riscv-none-embed-objdump.exe'
for filename in sys.argv[1:]:
    def disassemble(symbol):
        return subprocess.check_output([str(objdump), '-d', '--disassemble='+symbol, filename], text=True)
    dispatch = disassemble('SM_ParamInit')
    wrapper = disassemble('__wrap_smpInitiatorProcessIncoming')
    assert '<__wrap_smpInitiatorProcessIncoming>' in dispatch, 'SDK callback bypasses wrapper'
    assert '<smpInitiatorProcessIncoming>' in wrapper, 'wrapper does not delegate to real SDK'
    assert '22(a5)' in wrapper, 'expected identity type store absent; re-audit generated code'
    print(filename + ': SDK dispatch -> wrapper -> original handler verified')
