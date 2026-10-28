# Claude instructions for this repo

## Show the plan before changing code

Before creating, editing or deleting any code, describe the plan and wait for
the user to approve it.

- The plan says what will change and why, and lists the files to be added,
  modified or deleted.
- Do not touch files until the user approves. If they change the plan,
  show the revised plan first.
- Reading files, building, running tests and talking to the board are fine
  without approval. Changing files is not.
- Approval covers only the plan shown. Anything extra found along the way
  (a refactor, a convenience feature) goes in a new plan rather than in
  the same change.
- A direct instruction to make a specific change, such as "revert X",
  counts as approval for that change only.

## Report the files you touch

Every reply that creates, modifies, renames or deletes files must end with a
**Files touched** list covering that reply's changes:

```
Files touched:
- added    firmware/components/display/face.cpp
- modified firmware/main/main.cpp
- deleted  firmware/old_thing.cpp
```

- Use repo-relative paths, one per line, with `added`, `modified`, `renamed` or `deleted`.
- Include generated or config files you changed on purpose (e.g. `firmware/sdkconfig`), but not build output (`firmware/build/`, `managed_components/`).
- If a reply changes no files, leave the list out.
