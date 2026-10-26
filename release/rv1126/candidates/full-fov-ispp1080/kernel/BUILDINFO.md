# Build information

- Built: 2026-10-26 13:21 CST
- SDK: `/home/alientek/rv1126_sdk`
- Board configuration: `BoardConfig-alientek-rv1126.mk`
- Kernel configuration: `alientek_rv1126_defconfig`
- Device tree: `rv1126-alientek`
- Kernel release: Linux 4.19.111, build `#2 SMP PREEMPT`
- Candidate size: 7,447,552 bytes
- Candidate SHA-256:
  `7e45ac4916445ce2a304453bddccde72f1e1537a7b6e8c2e50305d31b69fdccd`

The board's stock 32 MiB boot partition was read back before any candidate
write:

- Local recovery copy: `/home/alientek/桌面/rv1126-stock-boot-20261026.img`
- Size: 33,554,432 bytes
- SHA-256:
  `1c7d396ae1f7e17d7bc77cdf4db7204e8ffb8879ef8e376b7a4f1fc66b6b06bf`

`dumpimage -l` confirms that stock and candidate use identical DTB and
resource payload hashes. Only the kernel payload changes:

- stock kernel: 6,577,760 bytes;
- candidate kernel: 6,577,912 bytes.

The build completed successfully. The candidate has not been written to the
development board.
