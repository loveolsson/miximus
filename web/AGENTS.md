# Web client instructions

Read [../docs/architecture.md](../docs/architecture.md) and [../docs/development.md](../docs/development.md) before changing graph synchronization or node definitions.

- The native server owns graph structure, options, defaults, validation, and status. The Baklava graph is a synchronized projection.
- Keep `web/src/messages.ts` synchronized with native action/topic enums and payloads.
- Keep web node type strings, interface names, option keys/defaults, and connection conversions synchronized with native nodes.
- Use `server_sync.ts` patterns to suppress client/server feedback loops. Do not ignore origin-side connection removals because they may be server-generated displacement side effects.
- Node-status broadcasts are deltas. Merge them into existing status; config and explicit pulls provide full snapshots.
- Use `StatusDropdownInterface` for registry-backed lists and match the native status key exactly.
- Use focus-tracking inputs where server updates must not overwrite active user edits.
- Register new nodes and interface types centrally in `web/src/nodes/types.ts` and `interface_types.ts`.
- Format touched TypeScript/Vue files with Prettier and run `npm run build` before handoff.
