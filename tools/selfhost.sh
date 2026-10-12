# Run on Eunuchs by tools/selfhost.py (make selfhost), in /mnt/host: build
# user space and the kernel twice, the second time with the tcc the first
# build made. Logs: build/selfhost/stage1.log and stage2.log.
make -f user/build.mk OUT=build/selfhost/stage1 TCC=tcc > build/selfhost/stage1.log 2>&1 || exit 1
make -f kernel/build.mk OUT=build/selfhost/stage1 TCC=tcc >> build/selfhost/stage1.log 2>&1 || exit 1
make -f user/build.mk OUT=build/selfhost/stage2 TCC=build/selfhost/stage1/bin/tcc > build/selfhost/stage2.log 2>&1 || exit 2
make -f kernel/build.mk OUT=build/selfhost/stage2 TCC=build/selfhost/stage1/bin/tcc >> build/selfhost/stage2.log 2>&1 || exit 2
exit 0
