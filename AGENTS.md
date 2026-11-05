# Agent Guidelines for the QEMU Project

## Local Workflow

- Do not do anything you are not specifically asked to do.
- Explain every command before running it.
- Do not create ad-hoc Python scripts, heredocs, or one-off automation for routine commands or file edits. This is wasteful and error-prone.
- Use the existing project scripts under `run/` (for example `run/telnet.py`, `run/mon.py`, and `run/console.py`) and the normal shell utilities.
- Use the `edit` and `write` tools for file changes, not inline scripting.
- Add a new helper script only when it is genuinely reusable, and prefer extending an existing helper over creating another one-off.
