use std::collections::BTreeMap;

use rmcp::schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use serde_json::Value;

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
pub struct OpenProjectRequest {
    /// Absolute path to a Shotcut .mlt or .xml project.
    pub path: String,
    /// Allow replacing an unsaved modified project without a save prompt.
    #[serde(default)]
    pub discard_unsaved: bool,
    /// Revision returned by project_snapshot or editor_status.
    pub expected_revision: i64,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
pub struct SaveProjectRequest {
    /// Optional absolute destination. Omit to save the current project in place.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub path: Option<String>,
    /// Store media paths relative to the project when possible.
    #[serde(default = "default_true")]
    pub relative_paths: bool,
    /// Permit replacing an existing explicit destination other than the current project.
    #[serde(default)]
    pub overwrite: bool,
    /// Revision returned by project_snapshot or editor_status.
    pub expected_revision: i64,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
pub struct HistoryRequest {
    /// Number of undo or redo steps.
    #[serde(default = "default_one")]
    pub steps: u32,
    /// Revision returned by project_snapshot or editor_status.
    pub expected_revision: i64,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
pub struct ExportVideoRequest {
    /// Absolute output file path.
    pub target: String,
    /// Shotcut/MLT stock preset name. Omit to use the current Export panel settings.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub preset: Option<String>,
    /// Permit replacing an existing target file.
    #[serde(default)]
    pub overwrite: bool,
    /// Revision returned by project_snapshot or editor_status.
    pub expected_revision: i64,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
pub struct ExportStatusRequest {
    /// Optional absolute target path used to filter the job list.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub target: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
pub struct ApplyEditPlanRequest {
    /// Human-readable undo-history label.
    pub label: String,
    /// Revision returned by project_snapshot or editor_status.
    pub expected_revision: i64,
    /// Validate without changing the project.
    #[serde(default)]
    pub dry_run: bool,
    /// Ordered editing operations. Later operations observe earlier operations.
    pub operations: Vec<EditOperation>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum TrackKind {
    Audio,
    Video,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum TrimEdge {
    In,
    Out,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum FadeEdge {
    In,
    Out,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
pub struct SubtitleSpec {
    /// Start time in milliseconds.
    pub start_ms: i64,
    /// End time in milliseconds.
    pub end_ms: i64,
    pub text: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(tag = "op", rename_all = "snake_case")]
pub enum EditOperation {
    AddTrack {
        kind: TrackKind,
        /// Insert at this index; omit to append.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        index: Option<i32>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        name: Option<String>,
    },
    RemoveTrack {
        track: i32,
    },
    InsertMedia {
        /// Absolute path to an existing media file.
        path: String,
        track: i32,
        /// Timeline position in frames.
        position: i32,
        /// Optional source in point in frames.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        in_frame: Option<i32>,
        /// Optional inclusive source out point in frames.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        out_frame: Option<i32>,
    },
    MoveClip {
        from_track: i32,
        clip: i32,
        to_track: i32,
        /// Destination timeline position in frames.
        position: i32,
        #[serde(default)]
        ripple: bool,
    },
    TrimClip {
        track: i32,
        clip: i32,
        edge: TrimEdge,
        /// Positive removes frames; negative restores available source frames.
        delta_frames: i32,
        #[serde(default)]
        ripple: bool,
    },
    SplitClip {
        track: i32,
        clip: i32,
        /// Absolute timeline position in frames.
        position: i32,
    },
    RemoveClip {
        track: i32,
        clip: i32,
        /// Ripple-delete when true; lift and leave a gap when false.
        #[serde(default)]
        ripple: bool,
    },
    SetClipGain {
        track: i32,
        clip: i32,
        /// Gain in decibels, from -120 to 60.
        gain: f64,
    },
    SetClipFade {
        track: i32,
        clip: i32,
        edge: FadeEdge,
        duration_frames: i32,
    },
    AddTransition {
        track: i32,
        clip: i32,
        /// Absolute timeline position in frames.
        position: i32,
        #[serde(default)]
        ripple: bool,
    },
    SetTrackState {
        track: i32,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        name: Option<String>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        muted: Option<bool>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        hidden: Option<bool>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        composite: Option<bool>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        locked: Option<bool>,
    },
    AddFilter {
        track: i32,
        clip: i32,
        /// Filter unique id reported by Shotcut metadata or an existing filter snapshot.
        filter_id: String,
        #[serde(default)]
        parameters: BTreeMap<String, Value>,
    },
    SetFilterParameters {
        track: i32,
        clip: i32,
        filter_index: i32,
        parameters: BTreeMap<String, Value>,
    },
    AddSubtitleTrack {
        name: String,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        language: Option<String>,
    },
    ReplaceSubtitles {
        track: i32,
        items: Vec<SubtitleSpec>,
    },
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
pub struct FullVideoPromptArgs {
    /// Editing goal and narrative.
    pub goal: String,
    /// Newline-separated absolute source-media paths.
    pub sources: String,
    /// Optional target duration, such as "60 seconds".
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub target_duration: Option<String>,
    /// Optional creative direction.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub style: Option<String>,
    /// Optional absolute export path.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub output_path: Option<String>,
}

const fn default_true() -> bool {
    true
}

const fn default_one() -> u32 {
    1
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn edit_operations_use_a_tagged_wire_format() {
        let value = serde_json::json!({
            "op": "insert_media",
            "path": "C:/media/intro.mp4",
            "track": 0,
            "position": 0
        });
        let operation: EditOperation = serde_json::from_value(value).unwrap();
        assert!(matches!(operation, EditOperation::InsertMedia { .. }));
    }

    #[test]
    fn unknown_operations_are_rejected() {
        let value = serde_json::json!({"op": "run_shell", "command": "nope"});
        assert!(serde_json::from_value::<EditOperation>(value).is_err());
    }

    #[test]
    fn save_requires_a_revision_and_defaults_safely() {
        assert!(
            serde_json::from_value::<SaveProjectRequest>(serde_json::json!({})).is_err()
        );
        let request: SaveProjectRequest =
            serde_json::from_value(serde_json::json!({"expected_revision": 1})).unwrap();
        assert!(request.relative_paths);
        assert!(!request.overwrite);
    }
}
