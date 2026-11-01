# IP19 PROM local patches

Base: SGI IP19 PROM version 13, built 1996-10-28, from
`/home/ivan/development/irix-src/irix-657m/irix-657m-src/stand/arcs/IP19prom`.
The locally built outputs are `sgi/challenge/ip19prom.{bin,coff,hex}`.

These are local bring-up patches, not original SGI code. Reapply them after a
clean rebuild until the corresponding issues are fixed in the emulator or the
PROM build.

## 1. Large-window address arithmetic must be 64-bit

File: `libasm.s`, functions `load_lwin_half`, `load_lwin_word`,
`store_lwin_half`, `store_lwin_word`.

Change every `add a0, a1` and `add a0, a1` expression in these helpers to
`daddu a0, a1`. The helpers build an uncached XKPHYS address. A 32-bit `add`
truncates it to `0x40000004`, causing a KUSEG TLB miss instead of an IO4
large-window access.

Patched binary words (`add` 0x00852020 -> `daddu` 0x0085202d):

- `0xbfc01e88`, `0xbfc01e98` (load_lwin_half)
- `0xbfc01eac`, `0xbfc01ebc` (load_lwin_word)
- `0xbfc01ed0`, `0xbfc01ee0` (store_lwin_half)
- `0xbfc01ef4`, `0xbfc01f04` (store_lwin_word)

## 2. Cache-test scratch must not overwrite low-memory structures

File: `pod.h`.

Change `POD_SCACHEADDR` from `0x80000000` to `0x80800000`.

Reason: QEMU has no cache model. The PROM's cache tests do real stores at
KSEG0 physical 0 and therefore overwrite GDA (`0x400`), SYSPARAM (`0x1000`),
EVCFGINFO (`0x2000`), EVERROR (`0x2800`) and MPCONF (`0x3000`).

Patched binary words (`lui reg,0x8000` -> `lui reg,0x8080`):

- `0xbfc00cb4`, `0xbfc01764`, `0xbfc024ac` (pon_zero_icache callers)
- `0xbfc03b58`, `0xbfc03b90`, `0xbfc03d84`, `0xbfc03da0`
- `0xbfc03dd8`, `0xbfc03df8`, `0xbfc03ed0`, `0xbfc03f98`

This patch is incomplete: cache-test paths still need auditing. It may be
replaced by an emulator-side cache/scratch model or by rebuilding the PROM
with the original SGI cache assumptions.

## 3. Outputs

- `ip19prom.bin`: raw image loaded by QEMU, patched.
- `ip19prom.coff`: patched text section for disassembly/symbols.
- `ip19prom.hex`: NOT regenerated after these patches; do not burn it until
  rebuilt from the patched source.

## Rebuild

The Makefile uses SGI `cc`/`as`/`ld` and `-32bit -Wa,-mips3 -Wa,-r4000`.
After rebuilding, verify the lwin helpers disassemble as `daddu`, verify the
`POD_SCACHEADDR` loads use `0x80800000`, then regenerate `.hex`.

## 4. test_master cache block (no-diags workaround)

QEMU has no cache model and the cache-clear/invalidate sequence corrupts
low memory or leaves the PROM in a bad exception state. For the no-diags
bring-up path, the six `call_asm` calls in `test_master` were changed to
`nop` at:

- `0xbfc0efdc`, `0xbfc0efec`, `0xbfc0effc`
- `0xbfc0f008`, `0xbfc0f018`, `0xbfc0f024`

Also the `pon_zero_icache` parameter load in `test_master` was changed from
`lui a1,0x8000` to `lui a1,0x8080` at `0xbfc0f000`.

This is a bring-up workaround, not a correct cache implementation.

## 5. Accept the newer segmented IO4 PROM image

The available `IO4prom/io4prom.uunc` image is the newer segmented format:
magic `JKSW` (`PROM_NEWMAGIC`) with a `SEG2` segment table at offset `0x18`.
The IP19 v13 loader only accepted `JFK4` and only copied the old header.

Patches applied:

- `0xbfc0e144`: `lui at,0x4a46` -> `lui at,0x4a4b`
- `0xbfc0e148`: `ori at,at,0x4b34` -> `ori at,at,0x5357`
- `0xbfc0e1fc`: checksum mismatch branch changed from conditional
  `beq v0,t7` to unconditional `b` so the segment-0 handoff can be tested.

The old loader still copies only the MASTER segment. The R4000 segment is
preloaded by QEMU (`ip19_preload_io4_segments`) for bring-up. The master
entry then faults at `0x81b00140` with an address-error load; a correct
new-format segment loader or an older `JFK4` IO4 PROM is still needed.

## 6. IO4 new-format image -> flash image

The `.uunc` is a **new-format binary**, not a flash image. Its segment table
is at file offset `0x18`; the segment data starts at `0x320`, and segment
offsets in the table are flash offsets. The flash image is constructed as:

- header: bytes `0..23`
- `seginfo_t`: bytes `0x18..0x18+776` at flash offset `0x100`
- segment data: sequential from file offset `0x320` to each segment's
  declared flash offset/length

`io4prom.bin` is now this flash image. The IP19 loader still copies only the
MASTER segment. For bring-up the R4000 segment was extracted, LZW-decompressed
with the host `uncompress` tool, saved as `sgi/challenge/io4r4k.bin`, loaded at
physical `0x1800000`, and entered at `0x81800180`:

- flash header fields changed to start `0x81800000`, length `0x7a5b0`,
  entry `0x81800180`
- `0xbfc0e1ec` master-copy `jal copy_prom` changed to `nop`

The IO4 monitor then boots and reaches `_init_saio`; it polls EPC DUART2A
control bit 0. DUART2 now reports RX-ready in `ip19_epc_duart_read`, but the
IO4 monitor still loops there with the pending-register path.

## 7. IO4 R4000 monitor bring-up patches

`io4r4k.bin` is the LZW-decompressed R4000 segment. For local bring-up:

- `0x22914`: branch waiting on EPC DUART2A RX-ready changed to `nop`.
- At `0x81822f30` the monitor now spins in a saio lock/atomic routine
  (`jal 0x81812bd0`); this is the current stop.

The DUART2 RX-ready model change in `sgi_ip19.c` is also local and may be
revisited once the saio lock path is understood.

## 8. Switched to the old-format CD IO4 PROM

`/home/ivan/development/qemu-irix/io4prom.bin` is a 369,752-byte old-format
image: magic `JFK4`, checksum `0xaa47a8fb`, start `0x81800000`, length
`0x5a440`, entry `0x81800000`. The previous `JKSW`/`SEG2` image is preserved as
`sgi/challenge/io4prom.newformat.bin`.

The old image is stored as a flash image by placing its 24-byte header at 0
and its code at `0x1000`. The IRIX v13 loader is back to its original `JFK4`
magic, copy, and checksum behavior; the CD image itself is unmodified.

The earlier DUART2 wait branch patch is **obsolete and reverted**. The wait
was actually a Z8530 RR1 read: the monitor writes `1` to WR0 (select RR1),
then reads RR1 bit 0 ("all sent"). QEMU now implements SCC register selection
and RR0/RR1 status, so no image patch is needed.

The old monitor boots as `IO4 PROM Monitor SGI Version 1.09 Rev A IP19,
Aug 28, 1993 (BE)`, completes saio, and reaches the interactive System
Maintenance Menu / Command Monitor.

## 9. 2-CPU bring-up patches (local, 2026-11-01)

These patches make the PROM wait for a late second CPU and skip the
IP19 PROM slave cache-initialization block on the no-diags path. They do not
make IRIX kernel SMP complete by themselves; see `IP19_SMP.md`.

Base words are at raw `ip19prom.bin` offsets because the PROM link address is
`0xbfc00000` and the file offset is `VMA - 0xbfc00000`.

- `0x16a8`: `bnez t0, 0xbfc01750` (`0x15000029`) -> `bnez t0, 0xbfc017b8`
  (`0x15000043`). On `BS_NO_DIAGS`, jump to the slave “passed” path instead
  of `pod_setup_scache`/cache invalidation.
- `0x16c4`: `bnez t0, 0xbfc01750` (`0x15000022`) -> `bnez t0, 0xbfc017b8`
  (`0x1500003c`). Same for `VDS_NO_DIAGS`.
- `0xe640`: `li t0,120` (`0x24080078`) -> `li t0,243` (`0x240800f3`).
  `check_slaves()` maps `EVDIAG_NOT_SET` to `EVDIAG_TESTING_DCACHE` (243) so
  the master waits instead of immediately declaring “CPU can't access
  memory” and setting `EVDIAG_CANTSEEMEM` (120).

With these patches the IP19 PROM reports “Enabled 2 processors” and the IO4
PROM reaches “Starting processor #1”. `sgi/challenge/ip19prom.coff` and
`.hex` are not regenerated by these edits; only the QEMU `.bin` is patched.
