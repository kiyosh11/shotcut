use std::collections::BTreeMap;

use rmcp::schemars::{self, JsonSchema};
use serde::{Deserialize, Serialize};

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
    /// Absolute output file path. Image-sequence paths must not contain a `%` token because
    /// Shotcut generates the sole frame-number token.
    pub target: String,
    /// Shotcut/MLT preset name; advertised presets remain subject to the final consumer policy,
    /// including exact canonical allowlisted muxer tokens.
    /// Omit to use Shotcut's MCP-safe default export settings.
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
    /// Ordered editing operations. Real plans resolve indices and validate operations
    /// sequentially inside one undo transaction. Dry runs do not mutate the project, so a
    /// potentially invalidating operation must be last and longer dry-run workflows must be
    /// staged against fresh snapshots.
    pub operations: Vec<EditOperation>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
pub struct EditorControlRequest {
    /// Revision returned by project_snapshot or editor_status. This prevents selection commands
    /// from addressing a clip after the timeline changed.
    pub expected_revision: i64,
    pub command: EditorCommand,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(tag = "action", rename_all = "snake_case")]
pub enum EditorCommand {
    Seek {
        /// Absolute timeline position in frames.
        position: i32,
    },
    SeekRelative {
        /// Signed number of frames to move from the current playhead.
        frames: i32,
    },
    SelectClip {
        track: i32,
        clip: i32,
    },
    SelectTrack {
        track: i32,
    },
    ClearSelection,
    Play {
        /// Playback rate. Negative values play in reverse; zero is not accepted.
        #[serde(default = "default_playback_speed")]
        speed: f64,
    },
    Pause,
    Stop,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
pub struct SetProjectProfileRequest {
    /// Revision returned by project_snapshot or editor_status.
    pub expected_revision: i64,
    /// Even frame width in pixels.
    pub width: u32,
    /// Even frame height in pixels.
    pub height: u32,
    pub frame_rate_num: u32,
    pub frame_rate_den: u32,
    pub progressive: bool,
    pub colorspace: ProfileColorspace,
    pub dynamic_range: ProfileDynamicRange,
    /// Optional display-aspect numerator. Supply both display-aspect fields or neither; omitted
    /// values use square pixels and the frame dimensions.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub display_aspect_num: Option<u32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub display_aspect_den: Option<u32>,
    /// Must be true when a loaded project has undo history. Shotcut reloads the in-memory project
    /// under the new profile and cannot safely retain history made under the old frame geometry.
    pub clear_undo_history: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum ProfileColorspace {
    Bt601,
    Bt709,
    Bt2020,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum ProfileDynamicRange {
    Sdr,
    Hlg,
    Pq,
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
#[serde(rename_all = "snake_case")]
pub enum KeyframeInterpolation {
    /// Compatibility alias for Shotcut's native discrete interpolation.
    Hold,
    Linear,
    /// Compatibility alias for Shotcut's native smooth-natural interpolation.
    Smooth,
    /// Compatibility alias for Shotcut's native cubic ease-in interpolation.
    EaseIn,
    /// Compatibility alias for Shotcut's native cubic ease-out interpolation.
    EaseOut,
    /// Compatibility alias for Shotcut's native cubic ease-in-out interpolation.
    EaseInOut,
    Discrete,
    SmoothLoose,
    SmoothNatural,
    SmoothTight,
    EaseInSinusoidal,
    EaseOutSinusoidal,
    EaseInOutSinusoidal,
    EaseInQuadratic,
    EaseOutQuadratic,
    EaseInOutQuadratic,
    EaseInCubic,
    EaseOutCubic,
    EaseInOutCubic,
    EaseInQuartic,
    EaseOutQuartic,
    EaseInOutQuartic,
    EaseInQuintic,
    EaseOutQuintic,
    EaseInOutQuintic,
    EaseInExponential,
    EaseOutExponential,
    EaseInOutExponential,
    EaseInCircular,
    EaseOutCircular,
    EaseInOutCircular,
    EaseInBack,
    EaseOutBack,
    EaseInOutBack,
    EaseInElastic,
    EaseOutElastic,
    EaseInOutElastic,
    EaseInBounce,
    EaseOutBounce,
    EaseInOutBounce,
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
#[serde(untagged)]
pub enum FilterParameterValue {
    String(String),
    Boolean(bool),
    Integer(i64),
    Number(f64),
    Null,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(tag = "op", rename_all = "snake_case")]
pub enum EditOperation {
    AddTrack {
        kind: TrackKind,
        /// Insert at this logical timeline index. Video tracks must remain before audio tracks.
        /// Omit to use Shotcut's default position (top video or last audio).
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
    SetClipSpeed {
        track: i32,
        clip: i32,
        /// Constant playback-speed multiplier from 0.05 through 20.0.
        speed: f64,
        /// Preserve audio pitch while changing speed.
        #[serde(default)]
        preserve_pitch: bool,
        /// Ripple the changed duration through the track.
        #[serde(default = "default_true")]
        ripple: bool,
        /// Ripple synchronized tracks as well. Ignored unless ripple is true.
        #[serde(default)]
        ripple_all_tracks: bool,
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
    #[schemars(extend("anyOf" = [
        {"required": ["name"]},
        {"required": ["muted"]},
        {"required": ["hidden"]},
        {"required": ["composite"]},
        {"required": ["locked"]}
    ]))]
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
        parameters: BTreeMap<String, FilterParameterValue>,
    },
    SetFilterParameters {
        track: i32,
        clip: i32,
        filter_index: i32,
        #[schemars(extend("minProperties" = 1))]
        parameters: BTreeMap<String, FilterParameterValue>,
    },
    SetFilterState {
        track: i32,
        clip: i32,
        filter_index: i32,
        disabled: bool,
    },
    RemoveFilter {
        track: i32,
        clip: i32,
        filter_index: i32,
    },
    SetFilterKeyframe {
        track: i32,
        clip: i32,
        filter_index: i32,
        /// Keyframe-capable numeric property reported by the filter catalog.
        property: String,
        /// Position relative to the clip in frames.
        position: i32,
        value: f64,
        /// Required when creating a point. Omit when updating only the value of an existing
        /// point to preserve its exact native interpolation type.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        interpolation: Option<KeyframeInterpolation>,
    },
    RemoveFilterKeyframe {
        track: i32,
        clip: i32,
        filter_index: i32,
        property: String,
        /// Position relative to the clip in frames.
        position: i32,
    },
    AddMarker {
        text: String,
        /// Absolute timeline position in frames.
        start: i32,
        /// Optional exclusive range end. Omit for a point marker.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        end: Option<i32>,
        /// Optional #RRGGBB color. Omit to use the user's current Shotcut marker color.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        color: Option<String>,
    },
    UpdateMarker {
        marker_index: i32,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        text: Option<String>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        start: Option<i32>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        end: Option<i32>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        color: Option<String>,
    },
    RemoveMarker {
        marker_index: i32,
    },
    AddSubtitleTrack {
        name: String,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        language: Option<String>,
    },
    ReplaceSubtitles {
        track: i32,
        /// Complete replacement contents for this subtitle track. An empty list clears it.
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

const fn default_playback_speed() -> f64 {
    1.0
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
    fn absent_optional_fields_are_omitted_from_bridge_json() {
        let operation = EditOperation::AddTrack {
            kind: TrackKind::Video,
            index: None,
            name: None,
        };
        let value = serde_json::to_value(operation).unwrap();
        assert_eq!(value["op"], "add_track");
        assert!(value.get("index").is_none());
        assert!(value.get("name").is_none());
    }

    #[test]
    fn complex_filter_parameter_values_are_rejected() {
        let value = serde_json::json!({
            "op": "add_filter",
            "track": 0,
            "clip": 0,
            "filter_id": "example",
            "parameters": {"unsupported": [1, 2, 3]}
        });
        assert!(serde_json::from_value::<EditOperation>(value).is_err());
    }

    #[test]
    fn unknown_operations_are_rejected() {
        let value = serde_json::json!({"op": "run_shell", "command": "nope"});
        assert!(serde_json::from_value::<EditOperation>(value).is_err());
    }

    #[test]
    fn production_edit_defaults_are_explicit_and_safe() {
        let speed: EditOperation = serde_json::from_value(serde_json::json!({
            "op": "set_clip_speed",
            "track": 0,
            "clip": 1,
            "speed": 1.25
        }))
        .unwrap();
        assert!(matches!(
            speed,
            EditOperation::SetClipSpeed {
                ripple: true,
                ripple_all_tracks: false,
                preserve_pitch: false,
                ..
            }
        ));

        let keyframe: EditOperation = serde_json::from_value(serde_json::json!({
            "op": "set_filter_keyframe",
            "track": 0,
            "clip": 1,
            "filter_index": 0,
            "property": "level",
            "position": 12,
            "value": 0.5
        }))
        .unwrap();
        assert!(matches!(
            keyframe,
            EditOperation::SetFilterKeyframe {
                interpolation: None,
                ..
            }
        ));
    }

    #[test]
    fn native_keyframe_interpolations_round_trip_exactly() {
        const NATIVE_NAMES: [&str; 35] = [
            "discrete",
            "linear",
            "smooth_loose",
            "smooth_natural",
            "smooth_tight",
            "ease_in_sinusoidal",
            "ease_out_sinusoidal",
            "ease_in_out_sinusoidal",
            "ease_in_quadratic",
            "ease_out_quadratic",
            "ease_in_out_quadratic",
            "ease_in_cubic",
            "ease_out_cubic",
            "ease_in_out_cubic",
            "ease_in_quartic",
            "ease_out_quartic",
            "ease_in_out_quartic",
            "ease_in_quintic",
            "ease_out_quintic",
            "ease_in_out_quintic",
            "ease_in_exponential",
            "ease_out_exponential",
            "ease_in_out_exponential",
            "ease_in_circular",
            "ease_out_circular",
            "ease_in_out_circular",
            "ease_in_back",
            "ease_out_back",
            "ease_in_out_back",
            "ease_in_elastic",
            "ease_out_elastic",
            "ease_in_out_elastic",
            "ease_in_bounce",
            "ease_out_bounce",
            "ease_in_out_bounce",
        ];

        for name in NATIVE_NAMES {
            let interpolation: KeyframeInterpolation =
                serde_json::from_value(serde_json::json!(name)).unwrap();
            assert_eq!(
                serde_json::to_value(interpolation).unwrap(),
                serde_json::json!(name)
            );
        }
    }

    #[test]
    fn editor_control_is_a_closed_typed_command_set() {
        let request: EditorControlRequest = serde_json::from_value(serde_json::json!({
            "expected_revision": 4,
            "command": {"action": "seek_relative", "frames": -1}
        }))
        .unwrap();
        assert!(matches!(
            request.command,
            EditorCommand::SeekRelative { frames: -1 }
        ));
        assert!(serde_json::from_value::<EditorControlRequest>(serde_json::json!({
            "expected_revision": 4,
            "command": {"action": "run_shell"}
        }))
        .is_err());
    }

    #[test]
    fn project_profile_requires_every_format_choice() {
        let explicit: SetProjectProfileRequest =
            serde_json::from_value(serde_json::json!({
                "expected_revision": 9,
                "width": 1080,
                "height": 1920,
                "frame_rate_num": 30000,
                "frame_rate_den": 1001,
                "progressive": true,
                "colorspace": "bt709",
                "dynamic_range": "sdr",
                "clear_undo_history": true
            }))
            .unwrap();
        assert_eq!(explicit.width, 1080);
        assert_eq!(explicit.height, 1920);
        assert_eq!(explicit.frame_rate_num, 30000);
        assert_eq!(explicit.frame_rate_den, 1001);

        let missing_format_choice = serde_json::json!({
            "expected_revision": 9,
            "width": 1080,
            "height": 1920,
            "frame_rate_num": 30000,
            "frame_rate_den": 1001,
            "clear_undo_history": true
        });
        let missing_result =
            serde_json::from_value::<SetProjectProfileRequest>(missing_format_choice);
        assert!(missing_result.is_err());
    }

    #[test]
    fn save_requires_a_revision_and_defaults_safely() {
        assert!(serde_json::from_value::<SaveProjectRequest>(serde_json::json!({})).is_err());
        let request: SaveProjectRequest =
            serde_json::from_value(serde_json::json!({"expected_revision": 1})).unwrap();
        assert!(request.relative_paths);
        assert!(!request.overwrite);
    }
}
