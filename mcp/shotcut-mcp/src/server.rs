use rmcp::{
    ErrorData as McpError, RoleServer, ServerHandler, handler::server::wrapper::Parameters,
    model::*, prompt, prompt_handler, prompt_router, service::RequestContext, tool, tool_handler,
    tool_router,
};
use serde::Serialize;
use serde_json::{Value, json};

use crate::{
    bridge::{BridgeClient, BridgeError},
    schema::{
        ApplyEditPlanRequest, ExportStatusRequest, ExportVideoRequest, FullVideoPromptArgs,
        HistoryRequest, OpenProjectRequest, SaveProjectRequest,
    },
};

const INSTRUCTIONS: &str = "Control the live Shotcut editor through a local authenticated bridge. \
Always call editor_status and project_snapshot before writes. Pass the returned revision as \
expected_revision and dry-run edit plans before applying them. Editing, save, undo, redo, and \
export tools change local state or files and require user approval. Never invent track or clip \
indexes or paths; use snapshot data. Export only when the user explicitly requests it.";

#[derive(Clone)]
pub struct ShotcutServer {
    bridge: BridgeClient,
}

impl ShotcutServer {
    pub fn from_env() -> Result<Self, BridgeError> {
        Ok(Self {
            bridge: BridgeClient::from_env()?,
        })
    }

    async fn run_tool<P>(&self, method: &str, params: P) -> Result<CallToolResult, McpError>
    where
        P: Serialize,
    {
        match self.bridge.call(method, params).await {
            Ok(value) => Ok(CallToolResult::success(vec![ContentBlock::text(
                format_json(&value),
            )])),
            Err(error) => Ok(CallToolResult::error(vec![ContentBlock::text(
                error.to_string(),
            )])),
        }
    }
}

#[tool_router]
impl ShotcutServer {
    #[tool(
        description = "Read connection state, project path, revision, undo/redo state, allowed roots, installed clip filters, policy-gated export presets, and queued jobs.",
        annotations(
            read_only_hint = true,
            destructive_hint = false,
            idempotent_hint = true,
            open_world_hint = false
        )
    )]
    async fn editor_status(&self) -> Result<CallToolResult, McpError> {
        self.run_tool("editor.status", json!({})).await
    }

    #[tool(
        description = "Read the complete live timeline snapshot: tracks, clips, filters, subtitles, selection, profile, and revision.",
        annotations(
            read_only_hint = true,
            destructive_hint = false,
            idempotent_hint = true,
            open_world_hint = false
        )
    )]
    async fn project_snapshot(&self) -> Result<CallToolResult, McpError> {
        self.run_tool("project.snapshot", json!({})).await
    }

    #[tool(
        description = "Open an existing Shotcut .mlt or .xml project inside an allowed filesystem root.",
        annotations(
            read_only_hint = false,
            destructive_hint = true,
            idempotent_hint = false,
            open_world_hint = false
        )
    )]
    async fn open_project(
        &self,
        Parameters(request): Parameters<OpenProjectRequest>,
    ) -> Result<CallToolResult, McpError> {
        self.run_tool("project.open", request).await
    }

    #[tool(
        description = "Save the current project in place or to an explicit .mlt/.xml path. Uses optimistic revision checking.",
        annotations(
            read_only_hint = false,
            destructive_hint = true,
            idempotent_hint = true,
            open_world_hint = false
        )
    )]
    async fn save_project(
        &self,
        Parameters(request): Parameters<SaveProjectRequest>,
    ) -> Result<CallToolResult, McpError> {
        self.run_tool("project.save", request).await
    }

    #[tool(
        description = "Validate or apply an ordered, typed full-video edit plan as one Shotcut undo-history entry.",
        annotations(
            read_only_hint = false,
            destructive_hint = true,
            idempotent_hint = false,
            open_world_hint = false
        )
    )]
    async fn apply_edit_plan(
        &self,
        Parameters(request): Parameters<ApplyEditPlanRequest>,
    ) -> Result<CallToolResult, McpError> {
        self.run_tool("timeline.apply", request).await
    }

    #[tool(
        description = "Undo one or more Shotcut history entries after checking the expected project revision.",
        annotations(
            read_only_hint = false,
            destructive_hint = true,
            idempotent_hint = false,
            open_world_hint = false
        )
    )]
    async fn undo(
        &self,
        Parameters(request): Parameters<HistoryRequest>,
    ) -> Result<CallToolResult, McpError> {
        self.run_tool("history.undo", request).await
    }

    #[tool(
        description = "Redo one or more Shotcut history entries after checking the expected project revision.",
        annotations(
            read_only_hint = false,
            destructive_hint = true,
            idempotent_hint = false,
            open_world_hint = false
        )
    )]
    async fn redo(
        &self,
        Parameters(request): Parameters<HistoryRequest>,
    ) -> Result<CallToolResult, McpError> {
        self.run_tool("history.redo", request).await
    }

    #[tool(
        description = "Queue a Shotcut export for the current timeline using MCP-safe defaults or an advertised preset that passes final consumer policy, including exact canonical muxer validation.",
        annotations(
            read_only_hint = false,
            destructive_hint = true,
            idempotent_hint = false,
            open_world_hint = false
        )
    )]
    async fn export_video(
        &self,
        Parameters(request): Parameters<ExportVideoRequest>,
    ) -> Result<CallToolResult, McpError> {
        self.run_tool("export.start", request).await
    }

    #[tool(
        description = "Read queued, running, and completed Shotcut export jobs, optionally filtered by target path.",
        annotations(
            read_only_hint = true,
            destructive_hint = false,
            idempotent_hint = true,
            open_world_hint = false
        )
    )]
    async fn export_status(
        &self,
        Parameters(request): Parameters<ExportStatusRequest>,
    ) -> Result<CallToolResult, McpError> {
        self.run_tool("export.status", request).await
    }
}

#[prompt_router]
impl ShotcutServer {
    #[prompt(
        name = "edit_full_video",
        description = "Plan and execute a complete Shotcut edit from source media and creative direction"
    )]
    async fn edit_full_video(
        &self,
        Parameters(args): Parameters<FullVideoPromptArgs>,
        _context: RequestContext<RoleServer>,
    ) -> Result<GetPromptResult, McpError> {
        let mut constraints = Vec::new();
        if let Some(duration) = args.target_duration {
            constraints.push(format!("Target duration: {duration}"));
        }
        if let Some(style) = args.style {
            constraints.push(format!("Creative direction: {style}"));
        }
        if let Some(output) = args.output_path {
            constraints.push(format!("Requested output: {output}"));
        }
        let constraint_text = if constraints.is_empty() {
            "No additional constraints supplied.".to_owned()
        } else {
            constraints.join("\n")
        };

        Ok(GetPromptResult::new(vec![
            PromptMessage::new_text(
                Role::Assistant,
                "Act as a careful video editor. Inspect editor_status and project_snapshot first. \
Build edits from the reported frame rate and real track/clip indexes. Submit a dry-run plan using \
the snapshot revision, correct any validation errors, then ask for approval before applying. \
Re-read the snapshot after major stages. Save before export and never overwrite a target unless \
the user explicitly requested it.",
            ),
            PromptMessage::new_text(
                Role::User,
                format!(
                    "Editing goal:\n{}\n\nSource media:\n{}\n\n{}",
                    args.goal, args.sources, constraint_text
                ),
            ),
        ])
        .with_description("A safe inspect-plan-dry-run-apply-save-export workflow for Shotcut"))
    }
}

#[tool_handler]
#[prompt_handler]
impl ServerHandler for ShotcutServer {
    fn get_info(&self) -> ServerInfo {
        ServerInfo::new(
            ServerCapabilities::builder()
                .enable_tools()
                .enable_prompts()
                .enable_resources()
                .build(),
        )
        .with_server_info(Implementation::from_build_env())
        .with_instructions(INSTRUCTIONS)
    }

    async fn list_resources(
        &self,
        _request: Option<PaginatedRequestParams>,
        _context: RequestContext<RoleServer>,
    ) -> Result<ListResourcesResult, McpError> {
        Ok(ListResourcesResult {
            resources: vec![
                Resource::new("shotcut://editor/status", "Shotcut editor status"),
                Resource::new("shotcut://project/snapshot", "Shotcut project snapshot"),
            ],
            ..Default::default()
        })
    }

    async fn read_resource(
        &self,
        request: ReadResourceRequestParams,
        _context: RequestContext<RoleServer>,
    ) -> Result<ReadResourceResult, McpError> {
        let method = match request.uri.as_str() {
            "shotcut://editor/status" => "editor.status",
            "shotcut://project/snapshot" => "project.snapshot",
            _ => {
                return Err(McpError::resource_not_found(
                    "Unknown Shotcut resource",
                    Some(json!({"uri": request.uri})),
                ));
            }
        };

        let value = self
            .bridge
            .call(method, json!({}))
            .await
            .map_err(bridge_to_mcp_error)?;
        Ok(ReadResourceResult::new(vec![ResourceContents::text(
            format_json(&value),
            request.uri,
        )]))
    }
}

fn bridge_to_mcp_error(error: BridgeError) -> McpError {
    McpError::internal_error(error.to_string(), None)
}

fn format_json(value: &Value) -> String {
    serde_json::to_string_pretty(value).unwrap_or_else(|_| value.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    const TOOL_NAMES: [&str; 9] = [
        "editor_status",
        "project_snapshot",
        "open_project",
        "save_project",
        "apply_edit_plan",
        "undo",
        "redo",
        "export_video",
        "export_status",
    ];

    #[test]
    fn tool_router_registers_every_editor_tool() {
        let router = ShotcutServer::tool_router();
        assert_eq!(router.list_all().len(), TOOL_NAMES.len());
        for name in TOOL_NAMES {
            assert!(router.get(name).is_some(), "missing MCP tool {name}");
        }
    }

    #[test]
    fn prompt_router_registers_full_video_workflow() {
        let router = ShotcutServer::prompt_router();
        assert_eq!(router.list_all().len(), 1);
        assert!(router.has_route("edit_full_video"));
    }
}
