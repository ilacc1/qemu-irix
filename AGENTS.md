# Agent Guidelines for the QEMU Project

This project aims to implement everything necessary to enable Irix operating
system emulation in QEMU. The repository root is this directory; the QEMU
source tree lives in `qemu/`.

## Local Workflow

- Do not do anything you are not specifically asked to do.
- Explain every command before running it.
- Do not create ad-hoc Python scripts, heredocs, or one-off automation for routine commands or file edits. This is wasteful and error-prone.
- Use the existing project scripts under `run/` (for example `run/telnet.py`, `run/mon.py`, and `run/console.py`) and the normal shell utilities.
- Use the `edit` and `write` tools for file changes, not inline scripting.
- Add a new helper script only when it is genuinely reusable, and prefer extending an existing helper over creating another one-off.
- Keep scratch files out of the source tree: use `/tmp` (preferably `/tmp/qemu-irix`). Only if a job needs more space than `/tmp` allows, use `/home/ivan/development/qemu-irix/tmp` and clean it afterwards.

## Reference material

As an example of working Irix emulation, we have:

- iris - a fully working, simpler emulator for irix
- mame in archive/reference/mame/source with full source code.
  - archive/reference/mame/run holds the working setup: run.sh launches indy_4020 with irix65_4020.chd.

These reference directories MUST NOT be written to.

Reference source trees:

- Mame - to be used as reference only in archive/reference/mame/source to be cloned from https://github.com/mamedev/mame
  - Mame build for Irix specifically can be done as in archive/reference/mame/source/build-for-irix.sh
- iris in ../iris/
- Irix kernel source in ../irix-src

## Debugging and tracing

Preferred approach is to conjecture where the issue may be in code, then do a
comparison against reference, ../iris and mame.

When that approach doesn't help, use tracing. You have created helper scripts
under `run/` for this purpose.

You can also use comparative tracing with mame and iris. You have a mame
instance for this in scratch, do not alter in any way reference/ folder.

If all else fails, using gdb is allowed, and use of screengrabs/OCR for
debugging visuals.

## General instructions

Reduce textual explanations and thinking output to a minimum but maintain
discipline of providing short one-sentence explanation for each command you
issue.
Do research via web searches when needed.
Execute instructions precisely, do not go beyond what's requested in the prompt.
Use available vector databases and mcp servers.
Never claim work is complete until the code builds and runs Irix operating
system reliably, which will be verified by operator.
Do not repeat yourself. Assume that once you've explained something, you do
not need to explain again.

## Working agreement (operator's standing instructions - follow every turn)

0. Explain every stage in one sentence before acting (what and why).
   No tool calls without that sentence first.

0. archive/reference/mame/source holds uncommitted changes that you are not to
   interfere with. Never run any git command on it under any circumstances (no
   status, diff, checkout, log, stash, fsck, or anything else). Never edit,
   build in, or otherwise change it in any way. To instrument or trace MAME,
   copy the source tree plus the root build script to an alternate location and
   change and build only there. Same ban applies to every other MAME tree copy.

1. Scratch discipline: all temporary files go in /tmp/qemu-irix (create it if
   missing) and build with TMPDIR=/tmp/qemu-irix. Only when a job needs more
   space than /tmp/qemu-irix may use, use the workspace-local ./tmp directory
   (/home/ivan/development/qemu-irix/tmp); nothing else may be created in
   project subfolders. Never scatter temp files across the home folder, /tmp
   root, or project subfolders. Maintain scratch space continuously: delete
   each run's logs and one-off files when done, keep only the current run's
   artifacts. Never fill /tmp to quota; check `du` before big extractions.
   Same discipline for emulator instances: never leave a QEMU or MAME process
   running behind (check `pidof qemu-system-mips64 indy_indigo2` at the end of
   every turn and kill strays); record the PID of any run you start so you can
   stop exactly that instance.

2. Status updates: after each unit of work, report ONE sentence, plain words,
   saying what changed and what it means (e.g. "Keyboard now reaches the PROM
   menu; sash still to do."). Detail only on request.

3. Code comments: write for a human reader. Short, plain English, and explain
   WHY, not what the code already says. Rules:
   - One or two lines per comment in most cases; a short paragraph only when
     the reasoning is genuinely non-obvious.
   - No changelogs in code (no "agent X added/fixed Y", no dates, no
     before/after stories, no "TEMP"/"FIXME" markers left behind).
   - No restating the code ("increment x by 1" above `x++`).
   - Keep the MAME source reference where the logic is ported from, in the
     compact form `MAME <file>:<line>` - that is the proof of provenance.
   - Delete a comment rather than keep a confusing one.

4. Code properly the first time. Most of our long debugging sessions came from
   first drafts written from guesses instead of from the MAME reference. Hence:
   - Before writing a device or protocol path, read the corresponding MAME
     source end to end and note the exact register/sequence semantics.
   - Port the logic faithfully (same state machine, same edge cases), don't
     invent shortcuts; where QEMU APIs force a different shape, say so in
     one comment line.
   - Self-review the diff before building: re-read every hunk and ask "what
     guest-observable behavior does this change, and did I verify it?".
   - Verify each change at runtime before moving on (boot to the same point
     as before plus the new behavior); never stack untested changes.
   - Bugs will still happen - but a faithful port fails loudly in one place,
     while a guessed one fails mysteriously in five.

5. Track everything in local git. The operator reviews history, so:
   - The git repository root is this directory; QEMU source is in `qemu/`.
     Work on branch `sgi-ip22-local` (never on master), commit finished units
     of work with one-line messages stating what guest behavior changed.
   - Scripts and documentation live at the repository root and are committed
     there, together with the QEMU changes.
   - Commit only clean states: builds green, `git diff --check` clean, no
     TEMP/debug leftovers, no scratch files, no VM images or logs.

6. Capture real console text wherever possible. OCR of the VNC display
   misreads kernel messages, and a misread message sends the whole
   investigation down the wrong path. Prefer, in order:
   - the kernel serial console: run with `IRIX_ESCC_LOG=<dir> ./run-irix.sh`,
     then in the PROM Command Monitor `setenv console d` and `boot`, and
     read the resulting esccA/esccB log files;
   - the PIO console over telnet (`IRIX_TELNET_PORT`);
   - QEMU's own stdout/stderr, and the monitor socket.
   - ocr if nothing else can give you the needed input
   Drive the PROM with HMP `sendkey` or the telnet console, never by
   screenshotting it. OCR is a legitimate fallback when no console path
   exists (e.g. graphics-only output), but never act on OCR text as a
   diagnosis without confirming it from a real capture when one is available.

7. Analysis discipline: no throwaway code, no stacked conclusions.
   - Don't write a new inline script per question. Keep one maintained
     analyzer per job, test it once on a small sample (head of the log, one
     window) and sanity-check the output before trusting it on the full data.
   - Reuse the existing drivers and harnesses instead of writing duplicates of
     the serial-login / monitor / screenshot loop.
   - Verify each intermediate result before building the next conclusion on
     it: a misread dump, a wrong coordinate frame, or a stale log section
     invalidates everything downstream. When a finding flips (e.g. stale vs
     latest dump), say so immediately and re-check what depended on it.
   - One question, one careful pass. Prefer the ground truth already in the
     trace (VRAM dumps, per-command diagnostics) over new instrumentation.
