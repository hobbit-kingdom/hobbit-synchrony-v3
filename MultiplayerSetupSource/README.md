# FakeHobbitsPatcher

Distributes the fake-hobbit assets into every level folder and registers the
`FAKE_HOBBITS` layer, then cleans up after itself.

## What it does

Given a root folder containing `FAKE_HOBBITS.EXPORT`, `BILBOFAKE.NPCGEOM` and a
`Levels\` directory, for **every** subfolder of `Levels\`:

1. Copies `FAKE_HOBBITS.EXPORT` and `BILBOFAKE.NPCGEOM` into it (overwriting).
2. In `ALLUSEDLAYERS.TXT` and `INITIALOBJECTLAYERS.TXT`, if a `"FAKE_HOBBITS"`
   entry is missing, appends it after the last layer entry and bumps the
   `[ Layers : N ]` counter by one.

When every folder is processed without errors, `FAKE_HOBBITS.EXPORT` and
`BILBOFAKE.NPCGEOM` are deleted from the root. If anything failed, the source
files are kept so nothing is lost.

## Usage

Drop `FakeHobbitsPatcher.exe` next to `Levels\` and double-click it, or:

```
FakeHobbitsPatcher.exe                 run against the current folder
FakeHobbitsPatcher.exe C:\path\to\root run against a specific folder
FakeHobbitsPatcher.exe --dry-run       report changes, write nothing
FakeHobbitsPatcher.exe --keep          don't delete the source files
FakeHobbitsPatcher.exe --help
```

With no folder argument the tool looks in the current directory, then walks up
from the exe's own location, picking the first folder that holds both source
files and a `Levels\` directory — so it also works from a subfolder.

Exit code is `0` on success, `1` if anything went wrong.

Output legend: `+` changed, `=` already correct (skipped), `.` file not present,
`~` warning, `!` error.

## Behaviour notes

- **Idempotent.** Re-running does nothing: entries already present are left
  alone and the counter is not bumped again.
- **Byte-preserving.** Files are read and written as ISO-8859-1, so untouched
  bytes stay identical. CRLF and LF line endings are each preserved as found.
- **Column-aligned.** The new entry copies the indent and column width of the
  existing entries in that file.
- **Counter source of truth** is the `[ Layers : N ]` header, not the number of
  lines. If the two disagree the tool warns and still bumps the header value, so
  a pre-existing miscount is never silently "fixed" into a different number.
- Level folders missing a layer file are reported and skipped, not treated as an
  error. A layer file with no entries at all is an error and is left untouched.

## Building

```
build.bat
```

Uses `csc.exe` from the .NET Framework that ships with Windows — no SDK, NuGet
or project files needed. Produces a ~12 KB standalone exe that runs on any
Windows 7 or newer.
