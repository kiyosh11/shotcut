# Shotcut MCP editor

This directory contains a pure-Rust [Model Context Protocol](https://modelcontextprotocol.io/) server that lets an MCP-capable AI operate a live Shotcut session. The AI model stays in the MCP client; this project does not embed a model, provider SDK, API key, or cloud service.

The Rust server communicates over standard input/output with the MCP client and over a separate authenticated local socket with Shotcut. Shotcut remains responsible for media decoding, its timeline model, undo history, filters, subtitles, project files, and export jobs.

## Capabilities

The server exposes these MCP tools:

- editor_status and project_snapshot inspect the live editor, timeline, clips, bundled clip-filter catalog, attached filters, subtitles, selection, export presets, and undo revision.
- open_project and save_project load and save Shotcut .mlt or .xml projects.
- apply_edit_plan validates or applies up to 500 typed operations as one undoable transaction.
- undo and redo use Shotcut's native history.
- export_video queues an export through Shotcut's job queue using MCP-safe defaults or an advertised preset that passes final consumer policy.
- export_status reports queued, running, completed, and failed exports.

Typed edit operations cover tracks, media insertion, clip moves/trims/splits/removal, clip gain and fades, transitions, track state, filters, and subtitle tracks/items. The edit_full_video prompt guides an AI through a complete inspect, plan, dry-run, apply, save, and export workflow.

## Safety model

The bridge is disabled unless SHOTCUT_MCP_ENABLE=1. When enabled it:

- accepts only same-user local socket or named-pipe connections;
- requires a session token of at least 32 characters on every request;
- restricts direct project, media, save, export, and file-valued filter paths to configured filesystem roots, validates supported nested MLT resource paths, and admits only bundled filter metadata plus a fixed set of Shotcut structural transitions;
- resets an omitted export preset to MCP-safe defaults; `editor_status.export_presets` is a discoverable but policy-gated list, and unsupported preset properties require manual export;
- validates the resolved consumer target after preset selection, requires exact canonical allowlisted muxer tokens, generates the sole image-sequence frame token, and performs bounded checks of existing sequence frames and their overwrite policy;
- exposes typed editor operations and never exposes a shell or arbitrary command execution;
- uses optimistic revision checks so an AI cannot silently overwrite newer manual edits;
- supports dry-run validation, refuses to overwrite native redo history, and removes a failed plan from history after reverting it;
- marks modifying MCP tools as destructive so compatible clients can require approval.

This is powerful editor access. Use a fresh token for each session, keep write approvals enabled, and restrict allowed roots to the media directories needed for the job.

MCP project opening and export are deliberately noninteractive. Projects that require repair, missing-file relinking, processing-mode conversion, or auto-save recovery must be handled in Shotcut first. Export likewise returns an error when media is missing or a filter analysis is pending; complete the analysis in Shotcut and retry.

MCP intentionally rejects non-null writes to the `html` and `resource` properties of `richText` and `qtext` filters because embedded markup can load external files. Use plain `argument` and styling properties, or insert pre-rendered media from an allowed root.

Noninteractive project opening also rejects network URLs (including UNC paths and remote-host `file://` URLs), known or content-sniffed playlist and manifest formats, active markup/animation loaders, WebVfx resources, and existing rich-text `html` or `resource` content because their transitive access cannot be safely enumerated. MLT loader files and explicit XML producer resources are checked recursively; cycles, DTD/entities, inline XML, consumers, links, external profiles, secondary roots, and relative project `root` attributes fail closed. Ordinary relative media resources remain supported when MLT defines them relative to the project directory or a canonical absolute root, and ordinary contained XML filter resources are not misclassified as MLT projects. Unknown or active producer loader services, extension-provided or unknown filter services, unknown transition services, active GPU shader and model-directory filters, unsupported dynamic AVFilter services and unapproved AVFilter options fail closed. JACK, JackRack, LADSPA, LV2, VST2, and OpenFX filter ID and service families are manual-only and are excluded from the catalog, add, live-edit, and project-opening paths. Nested mask selectors use exact raw safe tokens; Mask: Apply permits only an empty selector or exact `qtblend`. Affine backgrounds permit only exact lowercase `color:` or `colour:` producers with `0` or 3-, 4-, 6-, or 8-digit ASCII hexadecimal color payloads. Explicit Dust producer factories, luma/composite producer factories other than empty or exact `loader`, and sensitive nested transition producer/luma paths fail closed. The MCP filter catalog and parameter editor also exclude links, extension-generated metadata, and user filter sets; those and projects that need other rejected compatibility features can still be handled manually in Shotcut.

The noninteractive checker validates the original project before compatibility rewriting, disables proxy discovery and legacy WebVfx file loading during that pass, validates any rewritten temporary project, and revalidates the exact file immediately before MLT opens it. Aggregate nested XML size, depth, element, property, and resource counts are bounded. Dynamic query, fragment, glob, and all-files path syntax is rejected. Image-sequence reads require one well-formed printf token and at least one member; directory scans and member counts are bounded, and every member is canonicalized and inspected. Direct MCP writes to file-valued filter parameters must use explicit absolute local files inside an allowed root; project XML may use contained relative references where MLT defines them relative to the project.

Allowed roots are an application-level guard, not an operating-system sandbox around third-party media decoders. MCP rejects known text manifests on insertion, but opaque binary media formats may contain decoder-specific references that Shotcut cannot enumerate in advance. Use trusted media files and keep normal MCP write approvals enabled.

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

`SHOTCUT_BUILD_MCP_SERVER` defaults to `OFF`. When enabled, CMake requires an existing `cargo` executable and fails clearly if it cannot find one. CMake itself does not invoke a Rust installer, substitute a prebuilt sidecar, or download an upstream Shotcut binary. If `cargo` is managed by rustup and the pinned Rust 1.97.0 toolchain is missing, rustup may download and install it when Cargo starts; preinstall that toolchain or configure rustup for offline use when builds must not access the network. Cargo can also fetch the exact locked crates through its configured registry; use a vendored or offline Cargo configuration to prevent that.

The custom CMake target builds into `<build>/mcp-target/release`. Installation places the sidecar beside the Shotcut executable on Windows, in the application bundle on macOS, or in the configured binary directory on Unix.

The repository's Windows portable workflow enables this target with a pinned GNU-host Rust toolchain. It rejects a package unless the sidecar is nonempty and appears exactly once in the ZIP. Including the sidecar does not enable editor control: SHOTCUT_MCP_ENABLE=1, a fresh token, allowed roots, and explicit MCP client registration are still required.

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

Filter operations use IDs from the bundled filter catalog returned by editor_status. Media analysis, transcription, or generative assets can be performed by other approved MCP tools, then inserted through this server as files under an allowed root.

## Development status

GitHub Actions checks Rust formatting, Clippy warnings, and tests against the committed lockfile and pinned toolchain. The optional CMake target provides a reproducible build/install path without installing toolchains. The Windows portable workflow also launches the freshly extracted ZIP with isolated settings, exercises Elements thumbnail generation through software-rendered D3D11 and OpenGL, and rejects crashes or hangs before artifact upload. Timeline, save, export, and model-driven editing still require broader platform-specific integration testing.
