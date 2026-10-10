# Attach to `make debug` and load Eunuchs GDB helpers.
set architecture riscv:rv64
set confirm off
set pagination off
target remote localhost:1234
source tools/gdb/eunuchs.py
