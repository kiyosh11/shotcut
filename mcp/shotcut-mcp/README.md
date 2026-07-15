# Shotcut MCP editor

This directory contains a pure-Rust [Model Context Protocol](https://modelcontextprotocol.io/) server that lets an MCP-capable AI operate a live Shotcut session. The AI model stays in the MCP client; this project does not embed a model, provider SDK, API key, or cloud service.

The Rust server communicates over standard input/output with the MCP client and over a separate authenticated local socket with Shotcut. Shotcut remains responsible for media decoding, its timeline model, undo history, filters, subtitles, project files, and export jobs.

## Capabilities

The server exposes these MCP tools:

- editor_status and project_snapshot inspect the live editor, timeline, clips, installed clip-filter catalog, attached filters, subtitles, selection, export presets, and undo revision.
- open_project and save_project load and save Shotcut .mlt or .xml projects.
- apply_edit_plan validates or applies up to 500 typed operations as one undoable transaction.
- undo and redo use Shotcut's native history.
- export_video queues an export with Shotcut's existing Export panel and job queue.
- export_status reports queued, running, completed, and failed exports.

Typed edit operations cover tracks, media insertion, clip moves/trims/splits/removal, clip gain and fades, transitions, track state, filters, and subtitle tracks/items. The edit_full_video prompt guides an AI through a complete inspect, plan, dry-run, apply, save, and export workflow.

## Safety model

The bridge is disabled unless SHOTCUT_MCP_ENABLE=1. When enabled it:

- accepts only same-user local socket or named-pipe connections;
- requires a session token of at least 32 characters on every request;
- restricts project, media, save, and export paths to configured filesystem roots;
- exposes typed editor operations and never exposes a shell or arbitrary command execution;
- uses optimistic revision checks so an AI cannot silently overwrite newer manual edits;
- supports dry-run validation and rolls a failed edit plan back through Shotcut's undo stack;
- marks modifying MCP tools as destructive so compatible clients can require approval.

This is powerful editor access. Use a fresh token for each session, keep write approvals enabled, and restrict allowed roots to the media directories needed for the job.

## Environment

Shotcut and the Rust MCP process must receive the same token and endpoint. On Windows PowerShell, set the environment before starting the custom Shotcut build and the MCP client:

    $env:SHOTCUT_MCP_ENABLE = '1'
    $env:SHOTCUT_MCP_TOKEN = [guid]::NewGuid().ToString('N')
    $env:SHOTCUT_MCP_ALLOWED_ROOTS = "$HOME\Videos;$HOME\Documents"

Optional variables:

| Variable | Read by | Meaning |
| --- | --- | --- |
| SHOTCUT_MCP_ENDPOINT | both | Local socket or named-pipe name. It must match on both sides. |
| SHOTCUT_MCP_TIMEOUT_SECONDS | Rust server | Per-request timeout; defaults to 300 seconds. |
| SHOTCUT_MCP_ALLOWED_ROOTS | Shotcut | Existing root directories allowed for media and project I/O. Uses ; on Windows and : on Unix. |

If allowed roots are omitted, Shotcut permits the current user's home directory. An output file does not need to exist, but its parent directory must already exist inside an allowed root.

## MCP client configuration

Build the shotcut-mcp binary from this Cargo package when you are ready to compile the fork. Then copy [codex.example.toml](codex.example.toml) into the relevant part of your user or project Codex configuration and replace the command path.

The same standard-I/O MCP server can be registered with another MCP client using that client's local-server configuration. It needs SHOTCUT_MCP_TOKEN, and it optionally needs the endpoint and timeout variables.

For Codex configuration details, see the [official MCP documentation](https://learn.chatgpt.com/docs/extend/mcp). The server uses the [official Rust MCP SDK](https://github.com/modelcontextprotocol/rust-sdk).

## Full-video workflow

A capable AI should:

1. Call editor_status and project_snapshot.
2. Confirm the source paths and creative brief.
3. Submit apply_edit_plan with the current revision and dry_run=true.
4. Ask for write approval, then send the same plan with dry_run=false.
5. Re-inspect the timeline and correct issues using another revision-checked plan.
6. Save explicitly.
7. Export only after explicit approval, then poll export_status.

Filter operations use Shotcut filter IDs from the installed build. Media analysis, transcription, or generative assets can be performed by other approved MCP tools, then inserted through this server as files under an allowed root.

## Development status

The source integration is complete on this branch, but it has intentionally not been built or launched while being prepared. Before distributing it, compile Shotcut and this Cargo package in a normal development environment and run the focused Rust, bridge, timeline, save, and export checks.