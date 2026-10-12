/* With tests/guest/asmtest.S: built and run by tcc inside Eunuchs. */
#include <stdio.h>

long asm_sum(long n), asm_shifts(long x), asm_big(void), asm_table_sum(void);
long asm_atomic_inc(long *p), asm_call_twice(long x);

long twice(long x) { return 2 * x; }

int main(void)
{
    long v = 41;
    printf("sum=%ld shifts=%lx big=%lx table=%ld inc=%ld call=%ld\n", asm_sum(10),
           asm_shifts(-12345678901L), asm_big(), asm_table_sum(), asm_atomic_inc(&v),
           asm_call_twice(5));
    return 0;
}
