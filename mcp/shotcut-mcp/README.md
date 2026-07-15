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
- restricts project, media, save, export, and file-valued filter parameters to configured filesystem roots;
- exposes typed editor operations and never exposes a shell or arbitrary command execution;
- uses optimistic revision checks so an AI cannot silently overwrite newer manual edits;
- supports dry-run validation, refuses to overwrite native redo history, and removes a failed plan from history after reverting it;
- marks modifying MCP tools as destructive so compatible clients can require approval.

This is powerful editor access. Use a fresh token for each session, keep write approvals enabled, and restrict allowed roots to the media directories needed for the job.

MCP project opening and export are deliberately noninteractive. Projects that require repair, missing-file relinking, processing-mode conversion, or auto-save recovery must be handled in Shotcut first. Export likewise returns an error when media is missing or a filter analysis is pending; complete the analysis in Shotcut and retry.

MCP intentionally rejects non-null writes to the `html` and `resource` properties of `richText` and `qtext` filters because embedded markup can load external files. Use plain `argument` and styling properties, or insert pre-rendered media from an allowed root.

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

## Build and install

The sidecar is locked to the dependencies in `Cargo.lock` and the Rust version in `rust-toolchain.toml`. To build only the sidecar from this directory:

```sh
cargo build --release --locked
```

To build and install it together with a custom Shotcut build, enable the CMake option:

```sh
cmake -S /path/to/this/fork -B /path/to/build -DSHOTCUT_BUILD_MCP_SERVER=ON
cmake --build /path/to/build
cmake --install /path/to/build
```

`SHOTCUT_BUILD_MCP_SERVER` defaults to `OFF`. When enabled, CMake requires an existing `cargo` executable and fails clearly if it cannot find one. It never installs Rust, substitutes a prebuilt sidecar, or downloads an upstream Shotcut binary. Cargo can fetch the exact locked crates through its configured registry; use a vendored or offline Cargo configuration when builds must not access the network.

The custom CMake target builds into `<build>/mcp-target/release`. Installation places the sidecar beside the Shotcut executable on Windows, in the application bundle on macOS, or in the configured binary directory on Unix.

## MCP client configuration

Copy [codex.example.toml](codex.example.toml) into the relevant part of your user or project Codex configuration and replace the command with the absolute path to the custom `shotcut-mcp` binary produced from this fork. Official Shotcut binaries do not include the local MCP bridge and cannot be used as a substitute.

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

All edit indices are validated against the current snapshot. Structural changes that create, move, split, or remove indexed objects must be staged: apply them, then read a new snapshot before planning the next stage. Before applying a plan, resolve any existing redo history in Shotcut; the bridge refuses to destroy it.

Video tracks occupy the logical indexes before audio tracks. An explicit track insertion index must stay within that kind's region. When `index` is omitted, Shotcut adds a video track at the top or an audio track at the end.

Filter operations use Shotcut filter IDs from the installed build. Media analysis, transcription, or generative assets can be performed by other approved MCP tools, then inserted through this server as files under an allowed root.

## Development status

GitHub Actions checks Rust formatting, Clippy warnings, and tests against the committed lockfile and pinned toolchain. The optional CMake target provides a reproducible build/install path without installing toolchains. Full Shotcut bridge, timeline, save, and export behavior still requires platform-specific integration and runtime testing before distribution.
