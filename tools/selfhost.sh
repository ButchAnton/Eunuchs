# Eunuchs builds itself: user space and the kernel, twice, the second time
# with the tcc the first build made (the two must come out the same). Run it
# at the top of a source tree, which the image has in /usr/src/eunuchs:
#
#   cd /usr/src/eunuchs && sh tools/selfhost.sh
#
# The results go to $SH_OUT (default build/selfhost): stage1/ and stage2/,
# each with sysroot/, bin/ and kernel/kernel.elf, and stage1.log, stage2.log.
# Exit status 1 or 2: that stage failed (see its log). `make selfhost` on the
# Mac runs this in /usr/src/eunuchs with SH_OUT=/tmp/selfhost, copies the
# results back over the share and boots them.
O=${SH_OUT:-build/selfhost}
mkdir -p $O
make -f user/build.mk OUT=$O/stage1 TCC=tcc > $O/stage1.log 2>&1 || exit 1
make -f kernel/build.mk OUT=$O/stage1 TCC=tcc >> $O/stage1.log 2>&1 || exit 1
make -f user/build.mk OUT=$O/stage2 TCC=$O/stage1/bin/tcc > $O/stage2.log 2>&1 || exit 2
make -f kernel/build.mk OUT=$O/stage2 TCC=$O/stage1/bin/tcc >> $O/stage2.log 2>&1 || exit 2
exit 0
