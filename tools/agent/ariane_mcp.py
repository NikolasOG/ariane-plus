#!/usr/bin/env python3
"""MCP stdio adapter for Ariane's shared local agent service."""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Literal

from mcp.server import MCPServer
from mcp.server.mcpserver import Image
from PIL import Image as PILImage
from pydantic import BaseModel

if __package__:
	from .ariane_ipc import ArianeError, DEFAULT_ENGINE_SOCKET
else:
	from ariane_ipc import ArianeError, DEFAULT_ENGINE_SOCKET
if __package__:
	from .asset_index import DEFAULT_DATABASE
else:
	from asset_index import DEFAULT_DATABASE
if __package__:
	from .asset_discovery import DEFAULT_SESSION_DIR
else:
	from asset_discovery import DEFAULT_SESSION_DIR
if __package__:
	from .service import ArianeService, ScenePatchError, DEFAULT_STATE_DIR
else:
	from service import ArianeService, ScenePatchError, DEFAULT_STATE_DIR


service = ArianeService(
	Path(os.environ.get("ARIANE_ENGINE_SOCKET", DEFAULT_ENGINE_SOCKET)),
	Path(os.environ.get("ARIANE_ASSET_DB", DEFAULT_DATABASE)),
	discovery_dir=Path(os.environ.get("ARIANE_DISCOVERY_DIR", DEFAULT_SESSION_DIR)),
	state_dir=Path(os.environ.get("ARIANE_AGENT_STATE_DIR", DEFAULT_STATE_DIR)),
)
mcp = MCPServer(
	"ariane",
	description="Inspect GTA assets and safely edit an Ariane scratch layer.",
	instructions=(
		"SA-MP mapping: call mapping_toolkit_guide first - it lists the map_*, building_*, prefab_*, material_*, "
		"style_*, "
		"custom_model* and samp_sync tools (terrain scans, gap-free walls, packing, settling, lint, retexturing). "
		"Vibe mapping workflow: read project memory; survey the user camera/selected pixel without moving it. "
		"Translate the user's language into catalogue roles/styles/avoid while preserving their original brief. "
		"Read and annotate canonical asset previews before using uncertain fronts/anchors. "
		"Keep zones, access routes, semantic relationships, accepted choices and locks in project memory. "
		"Build a coherent palette, then block out large forms before adding small details. "
		"Use propose_asset_palette and render_palette_board for coherent visual selection. External semantic descriptions may be wrong. "
		"Use dimension-aware recipes where appropriate, inspect their orientation assumptions, resolve patches, "
		"inspect preflight, apply, render player-height and overview images, then fix concrete findings. "
		"Offer light variants when direction is ambiguous; do not exhaustively decorate every option. "
		"Review geometry and visual quality separately; never mark a visual review complete without seeing its images. "
		"Preserve locked objects, use edit history for selective undo and checkpoints for full variants. "
		"Saving is separate from committing. Do not claim HSV reference matching understands image semantics."
	),
)


class SceneOperation(BaseModel):
	"""One declarative edit; fields are validated again by Ariane before use."""
	action: Literal["place", "place_relative", "transform", "transform3d", "delete", "array_line", "array_ring"]
	key: str | None = None
	model: int | str | None = None
	instance_id: int | None = None
	group: str | None = None
	supported_by: str | None = None
	support_mode: Literal["declare_only", "snap"] = "declare_only"
	clearance: float = 0.0
	relative_to: str | None = None
	parent_anchor: str | None = None
	child_anchor: str | None = None
	relation: Literal["offset", "on_top", "in_front", "behind", "left", "right"] = "offset"
	gap: float = 0.0
	local_x: float = 0.0
	local_y: float = 0.0
	local_z: float = 0.0
	x: float | None = None
	y: float | None = None
	z: float | None = None
	x2: float | None = None
	y2: float | None = None
	z2: float | None = None
	radius: float | None = None
	count: int | None = None
	jitter: float = 0.0
	heading_noise: float = 0.0
	seed: str | int | None = None
	face: Literal["outward", "inward"] = "outward"
	pitch: float | None = None
	roll: float | None = None
	heading: float | None = None
	snap: bool = True


class SemanticZone(BaseModel):
	"""An explicit entrance, route, or keep-clear footprint in world coordinates."""
	name: str
	role: Literal["entrance", "circulation", "keep_clear"] = "keep_clear"
	shape: Literal["circle", "rect"]
	x: float
	y: float
	radius: float | None = None
	width: float | None = None
	depth: float | None = None
	heading: float = 0
	z_min: float | None = None
	z_max: float | None = None
	ignore_keys: list[str] = []
	severity: Literal["warning", "error"] = "error"


@mcp.tool()
def ariane_status() -> dict:
	"""Inspect engine capabilities, active scratch session, and scene bounds."""
	try:
		bounds = service.engine("scene_bounds")
	except ArianeError:
		bounds = None
	return {
		"capabilities": service.engine("capabilities"),
		"session": service.engine("session_status"),
		"bounds": bounds,
	}


@mcp.tool()
def search_assets(query: str, category: str | None = None,
	              max_width: float | None = None, max_depth: float | None = None,
	              has_collision: bool | None = None, defined_only: bool = True,
	              limit: int = 20, style: str | None = None, style_role: str | None = None,
	              style_mode: Literal["prefer", "limit"] = "prefer") -> list[dict]:
	"""Find GTA assets by text and physical constraints before visual review. style = a mapping style
	(styles_list): 'prefer' ranks its models (of style_role) first, 'limit' returns only them."""
	return service.search_assets(query, category=category, max_width=max_width,
	                             max_depth=max_depth, has_collision=has_collision,
	                             defined_only=defined_only, limit=limit, style=style,
	                             style_role=style_role, style_mode=style_mode)


@mcp.tool()
def inspect_asset(asset_id: int, ensure_renderable: bool = False) -> dict | None:
	"""Return catalogue metadata plus authoritative runtime availability and origin bounds."""
	return service.inspect_asset(asset_id, ensure_renderable=ensure_renderable)


@mcp.tool(structured_output=False)
def render_asset_views(asset_id: int, output_path: str, size: int = 512) -> list:
	"""Render two local canonical asset views with identity-axis provenance."""
	result = service.render_asset_views(asset_id, Path(output_path), size=size)
	return [result, Image(path=result["path"])]


@mcp.tool()
def similar_assets(asset_id: int, limit: int = 20) -> list[dict]:
	"""Find assets with matching GTA style, texture dictionary, and scale."""
	return service.assets.similar(asset_id, limit)


@mcp.tool()
def asset_contact_sheet(asset_ids: list[int], output_path: str, columns: int = 5) -> Image:
	"""Render labeled asset thumbnails and return the sheet for visual selection."""
	result = service.assets.contact_sheet(asset_ids, Path(output_path), columns=columns)
	return Image(path=result["path"])


@mcp.tool()
def start_asset_discovery(brief: str, roles: list[str] | None = None,
	                      styles: list[str] | None = None, avoid: list[str] | None = None,
	                      context_asset_ids: list[int] | None = None,
	                      max_width: float | None = None, max_depth: float | None = None,
	                      has_collision: bool | None = None, pool_limit: int = 2500) -> dict:
	"""Start a high-recall catalogue session and report auditable coverage.

	After observing the scene, put scene context in ``brief`` and explicitly list
	the prop roles wanted. Use ``avoid`` for visually inappropriate classes. The session
	considers every eligible non-LOD asset, retains diverse family representatives,
	and tracks what Codex has and has not visually reviewed.
	"""
	constraints = {key: value for key, value in {
		"max_width": max_width, "max_depth": max_depth, "has_collision": has_collision,
	}.items() if value is not None}
	structured_brief = {"scene": brief, "roles": roles or [], "styles": styles or [],
	                    "avoid": avoid or []}
	return service.discovery.start(structured_brief, context_asset_ids=context_asset_ids,
	                               constraints=constraints, pool_limit=pool_limit)


@mcp.tool()
def discovery_coverage(session_id: str) -> dict:
	"""Show reviewed assets/families, remaining branches, channels, and residual risk."""
	return service.discovery.coverage(session_id)


@mcp.tool()
def browse_asset_families(session_id: str, offset: int = 0, limit: int = 100) -> dict:
	"""List ranked visual/functional families without collapsing their members."""
	return service.discovery.families(session_id, offset=offset, limit=limit)


@mcp.tool()
def discovery_candidates(session_id: str, offset: int = 0, limit: int = 100) -> list[dict]:
	"""Return fused candidates with scores and independent retrieval-channel evidence."""
	return service.discovery.results(session_id, offset=offset, limit=limit)["assets"]


@mcp.tool()
def open_asset_family(session_id: str, family: str, offset: int = 0, limit: int = 64) -> list[dict]:
	"""Open one family, mark its page reviewed, and return full asset passports."""
	return service.discovery.open_family(session_id, family, offset=offset, limit=limit)["assets"]


@mcp.tool()
def render_discovery_atlas(session_id: str, output_path: str, family_limit: int = 64) -> Image:
	"""Render one representative from each high-priority family for Codex visual review."""
	result = service.discovery.atlas(session_id, Path(output_path), family_limit=family_limit)
	return Image(path=result["path"])


@mcp.tool()
def audit_discovery_residuals(session_id: str, limit: int = 64,
	                          strategy: Literal["stratified", "unknown"] = "stratified") -> list[dict]:
	"""Sample outside the candidate pool across families to expose retrieval blind spots."""
	return service.discovery.residuals(session_id, limit=limit, strategy=strategy)["assets"]


@mcp.tool()
def mark_discovery_assets(session_id: str, asset_ids: list[int],
	                      decision: Literal["shortlist", "rejected"], reason: str = "") -> dict:
	"""Persist Codex's visual selection or rejection rationale in the coverage ledger."""
	return service.discovery.mark(session_id, asset_ids, decision=decision, reason=reason)


@mcp.tool()
def discover_assets(brief: str, thoroughness: Literal["quick", "broad", "exhaustive"] = "quick",
	                roles: list[str] | None = None, styles: list[str] | None = None,
	                avoid: list[str] | None = None, limit: int = 40,
	                detail: Literal["compact", "full"] = "compact") -> dict:
	"""Discover diverse concepts; compact results are token-safe, full keeps complete passports."""
	return service.discover_assets(brief, thoroughness=thoroughness, roles=roles,
	                               styles=styles, avoid=avoid, limit=limit, detail=detail)


@mcp.tool()
def begin_edit_session(name: str = "agent-proposal") -> dict:
	"""Snapshot the active agent scene and begin an undoable scratch proposal."""
	return service.engine("session_begin", [name])


@mcp.tool()
def camera_context() -> dict:
	"""Read the exact live camera pose, basis, FOV, aspect ratio, and viewport."""
	return service.engine("camera_context")


@mcp.tool()
def screen_to_world(pixel_x: float, pixel_y: float,
	                viewport_width: int, viewport_height: int) -> dict:
	"""Resolve an image pixel to visible geometry or the terrain beneath that ray."""
	return service.engine("screen_to_world", [pixel_x, pixel_y, viewport_width, viewport_height])


@mcp.tool()
def raycast_segment(start_x: float, start_y: float, start_z: float,
	                target_x: float, target_y: float, target_z: float,
	                target_tolerance: float = 1.0) -> dict:
	"""Test whether world geometry occludes a segment without moving the live camera."""
	return service.client.raycast_segment(
		[start_x, start_y, start_z], [target_x, target_y, target_z],
		target_tolerance=target_tolerance)


@mcp.tool()
def apply_scene_patch(operations: list[SceneOperation], patch_id: str | None = None,
	                  expected_revision: int | None = None) -> dict:
	"""Apply a replay-safe patch using stable caller-owned keys.

	Set ``expected_revision`` after inspection to reject stale plans. Reusing the
	same ``patch_id`` returns the original receipt without duplicating mutations.
	"""
	try:
		return service.apply_scene_patch(
			[operation.model_dump(exclude_none=True) for operation in operations],
			patch_id=patch_id, expected_revision=expected_revision)
	except ScenePatchError as error:
		return error.payload


@mcp.tool()
def resolve_scene_patch(operations: list[SceneOperation],
	                    expected_revision: int | None = None) -> dict:
	"""Resolve and fully prevalidate a revision-bound plan without mutating the scene."""
	try:
		return service.resolve_scene_patch(
			[operation.model_dump(exclude_none=True) for operation in operations],
			expected_revision=expected_revision)
	except ScenePatchError as error:
		return error.payload


@mcp.tool()
def apply_resolved_plan(plan_id: str, patch_id: str | None = None) -> dict:
	"""Apply a prior resolved plan only if its scratch session and revision still match."""
	try:
		return service.apply_resolved_plan(plan_id, patch_id=patch_id)
	except ScenePatchError as error:
		return error.payload


@mcp.tool()
def create_scene_group(group: str, members: list[str | int]) -> dict:
	"""Create or replace a semantic group from stable keys or runtime IDs."""
	return service.create_group(group, members)


@mcp.tool()
def list_scene_groups() -> dict:
	"""List semantic groups in the active scratch composition."""
	return service.list_groups()


@mcp.tool()
def inspect_scene_group(group: str, offset: int = 0, limit: int = 50) -> dict:
	"""Inspect one revision-friendly page of a semantic group."""
	return service.inspect_group(group, offset=offset, limit=limit)


@mcp.tool()
def transform_scene_group(group: str, dx: float = 0.0, dy: float = 0.0,
	                      dz: float = 0.0, dheading: float = 0.0,
	                      patch_id: str | None = None,
	                      expected_revision: int | None = None) -> dict:
	"""Move and rotate a complete composition around its centroid."""
	return service.transform_group(group, dx=dx, dy=dy, dz=dz, dheading=dheading,
	                               patch_id=patch_id, expected_revision=expected_revision)


@mcp.tool()
def clone_scene_group(group: str, new_group: str, key_prefix: str,
	                  dx: float = 0.0, dy: float = 0.0, dz: float = 0.0,
	                  dheading: float = 0.0, patch_id: str | None = None) -> dict:
	"""Clone a composition with new stable keys and an optional rigid transform."""
	return service.clone_group(group, new_group, key_prefix, dx=dx, dy=dy, dz=dz,
	                           dheading=dheading, patch_id=patch_id)


@mcp.tool()
def delete_scene_group(group: str, patch_id: str | None = None) -> dict:
	"""Delete every live object in a semantic group."""
	return service.delete_group(group, patch_id=patch_id)


@mcp.tool()
def set_object_support(child: str | int, parent: str | int) -> dict:
	"""Declare that a child is intentionally supported by another scene object."""
	return service.set_support(child, parent)


@mcp.tool()
def snap_object_to_support(child: str | int, parent: str | int,
	                       clearance: float = 0.0) -> dict:
	"""Place a child on the collision top of a parent and record the support relation."""
	return service.snap_to_support(child, parent, clearance)


@mcp.tool()
def validate_composition() -> dict:
	"""Validate support, duplicates, and collisions with shelter containment semantics."""
	return service.validate_composition()


@mcp.tool()
def inspect_zone(x: float, y: float, radius: float, offset: int = 0,
	             limit: int = 64) -> dict:
	"""Inspect one bounded page of nearby native and scratch objects."""
	return service.engine("inspect_zone_page", [x, y, radius, offset, limit])


@mcp.tool()
def list_scene_objects(offset: int = 0, limit: int = 64,
	                   x: float | None = None, y: float | None = None,
	                   radius: float | None = None) -> dict:
	"""List one revisioned page of scratch objects, optionally scoped to a circle."""
	fields: list[object] = [offset, limit]
	if x is not None and y is not None and radius is not None:
		fields.extend([x, y, radius])
	return service.engine("list_page", fields)


@mcp.tool()
def analyze_placement(instance_id: int, grid_size: int = 3) -> dict:
	"""Measure terrain support, signed clearance, relief, normals, and slope below an object."""
	return service.engine("analyze_placement", [instance_id, grid_size])


@mcp.tool()
def fit_object_to_terrain(instance_id: int, profile: Literal["building", "prop"],
	                      search_radius: float = 30.0, search_step: float = 3.0,
	                      max_support_relief: float | None = None) -> dict:
	"""Relocate an upright building to flat support or ground-align a small prop."""
	fields: list[object] = [instance_id, profile, search_radius, search_step]
	if max_support_relief is not None:
		fields.append(max_support_relief)
	return service.engine("fit_terrain", fields)


@mcp.tool()
def validate_scene() -> dict:
	"""Run the primary validator for terrain, supports, duplicates, and overlaps."""
	return service.validate_composition()


@mcp.tool()
def validate_zone(x: float, y: float, radius: float, offset: int = 0,
	              limit: int = 64) -> dict:
	"""Validate one bounded page for floating, embedding, and unsupported slope."""
	return service.engine("validate_zone", [x, y, radius, offset, limit])


@mcp.tool()
def validate_semantic_zones(zones: list[SemanticZone]) -> dict:
	"""Detect props blocking explicitly declared entrances, routes, or clear areas."""
	return service.validate_semantic_zones([
		zone.model_dump(exclude_none=True) for zone in zones
	])


@mcp.tool()
def list_checkpoints() -> dict:
	"""List persistent scratch manifests available across Ariane restarts."""
	return service.checkpoint_list()


@mcp.tool()
def save_checkpoint(name: str) -> dict:
	"""Atomically persist the current scratch layer without saving the IPL."""
	return service.checkpoint_save(name)


@mcp.tool()
def restore_checkpoint(name: str) -> dict:
	"""Replace the active scratch proposal from a persistent checkpoint manifest."""
	return service.checkpoint_restore(name)


@mcp.tool(structured_output=False)
def capture_current_view(output_path: str, label: str = "current") -> list:
	"""Capture the untouched live viewport and return its exact pose provenance."""
	result = service.client.capture_current(Path(output_path), label=label)
	return [result, Image(path=result["path"])]


@mcp.tool(structured_output=False)
def capture_from_pose(output_path: str, label: str,
	                  position_x: float, position_y: float, position_z: float,
	                  target_x: float, target_y: float, target_z: float,
	                  up_x: float = 0.0, up_y: float = 0.0, up_z: float = 1.0,
	                  fov: float = 58.0,
	                  expected_camera_revision: int | None = None) -> list:
	"""Capture one chosen pose and conditionally restore the untouched live viewport."""
	result = service.client.capture_at_pose(
		Path(output_path), position=[position_x, position_y, position_z],
		target=[target_x, target_y, target_z], up=[up_x, up_y, up_z], fov=fov,
		label=label, expected_camera_revision=expected_camera_revision)
	return [result, Image(path=result["path"])]


@mcp.tool(structured_output=False)
def render_scene_views(output_directory: str, fov: float = 58.0,
	                   center_x: float | None = None, center_y: float | None = None,
	                   center_z: float | None = None, span: float | None = None) -> list:
	"""Render four provenance-bound views; an explicit center enables empty-scene survey."""
	center = None
	if center_x is not None and center_y is not None:
		center = [center_x, center_y, center_z if center_z is not None else 0.0]
	manifest = service.capture_views(Path(output_directory), fov, center=center, span=span)
	views = []
	for item in manifest["views"]:
		name, path = item["label"], item["path"]
		view = PILImage.open(path).convert("RGB")
		# Full-resolution captures stay on disk; the MCP review sheet is bounded so
		# one visual feedback pass does not consume tens of megabytes of context.
		view.thumbnail((960, 540), PILImage.Resampling.LANCZOS)
		views.append((name, view))
	width = max(view.width for _, view in views)
	height = max(view.height for _, view in views)
	sheet = PILImage.new("RGB", (width * 2, (height + 28) * 2), "#17191d")
	from PIL import ImageDraw
	draw = ImageDraw.Draw(sheet)
	for index, (name, view) in enumerate(views):
		x = index % 2 * width
		y = index // 2 * (height + 28)
		sheet.paste(view, (x, y))
		draw.rectangle((x, y + height, x + width, y + height + 28), fill="#111215")
		framing = manifest["views"][index].get("framing", {})
		angle = float(framing.get("azimuth_adjustment_degrees", 0.0))
		caption = name if abs(angle) < 0.5 else f"{name} · auto-orbit {angle:+.0f}°"
		draw.text((x + 8, y + height + 7), caption, fill="white")
	output = Path(output_directory).resolve() / "review-sheet.png"
	sheet.save(output)
	manifest["review_sheet"] = {"path": str(output), "tiles": [
		{"label": item["label"], "path": item["path"],
		 "actual_pose": item.get("actual_pose"),
		 "capture_camera_revision": item.get("capture_camera_revision"),
		 "framing": item.get("framing")}
		for item in manifest["views"]
	]}
	Path(manifest["manifest_path"]).write_text(
		__import__("json").dumps(manifest, indent=2), encoding="utf-8")
	return [manifest, Image(path=output)]


@mcp.tool()
def finish_edit_session(commit: bool) -> dict:
	"""Commit the visible proposal or restore the exact pre-session snapshot."""
	return service.engine("session_commit" if commit else "session_rollback")


@mcp.tool()
def save_scene() -> dict:
	"""Persist the committed agent IPL. Active scratch sessions cannot be saved."""
	return service.engine("save")


@mcp.tool()
def get_mapping_project() -> dict:
    """Read the persistent brief, palette, zones, relations, locks and creative phase."""
    return service.vibe.project()


@mcp.tool()
def update_mapping_project(changes: dict, expected_revision: int | None = None) -> dict:
    """Merge project fields. List fields replace their previous values. Preserve user choices and original brief."""
    return service.vibe.update_project(changes, expected_revision)


@mcp.tool(structured_output=False)
def survey_mapping_area(output_directory: str, radius: float = 30, grid: int = 7,
                        pixel_x: float | None = None, pixel_y: float | None = None) -> list:
    """Observe the camera target or selected pixel: annotated RGB, sampled depth/IDs, terrain and native objects."""
    result = service.vibe.observe(output_directory, radius, grid, pixel_x, pixel_y)
    # Full spatial payload stays available on disk; avoid duplicating hundreds of rays in the model context.
    return [{"center": result["center"], "scene_revision": result["scene_revision"],
             "objects": result["objects"][:64], "total_objects": result["total_objects"],
             "surface": result["surface"], "survey_path": str(Path(output_directory).resolve() / "survey.json"),
             "limitations": result["limitations"]}, Image(path=result["images"]["annotated.png"])]


@mcp.tool()
def list_composition_recipes() -> dict:
    """List supported parametric compositions and their required palette roles."""
    return service.vibe.dispatch("recipe.list", {})


@mcp.tool()
def build_composition_recipe(name: Literal["terrace", "market", "checkpoint", "alley", "fence", "camp", "logistics", "outpost", "garden"],
                             palette: dict[str, int], key: str, x: float, y: float,
                             heading: float = 0, count: int = 3, aisle: float = 3,
                             gap: float = 0.25, seed: str = "0",
                             points: list[list[float]] | None = None, gate_width: float = 0) -> dict:
    """Generate a dimension-aware patch without applying it. Merge its circulation constraints into project zones."""
    return service.vibe.recipe(name, palette, key=key, x=x, y=y, heading=heading,
                               count=count, aisle=aisle, gap=gap, seed=seed, points=points, gate_width=gate_width)


@mcp.tool()
def asset_passport(asset_id: int) -> dict:
    """Read measured catalogue metadata, sourced visual annotations, functional anchors and explicit unknowns."""
    return service.profiles.get(asset_id)


@mcp.tool()
def annotate_asset_passport(asset_id: int, features: dict, source: str, confidence: float = 0.7) -> dict:
    """Store visually observed features. Anchors are local xyz; front_heading is degrees from model +Y. Include image provenance."""
    return service.profiles.annotate(asset_id, features, source, confidence)


@mcp.tool()
def search_annotated_assets(query: str, limit: int = 30) -> list[dict]:
    """Search learned visual/functional annotations, including the user's own vocabulary."""
    return service.profiles.search(query, limit)


@mcp.tool()
def index_asset_reference_images(directory: str) -> dict:
    """Build local colour descriptors from MODEL_ID.png thumbnails, once per catalogue."""
    return service.profiles.index_images(directory)


@mcp.tool()
def find_assets_for_reference(image_path: str, limit: int = 30, candidates: list[int] | None = None) -> dict:
    """Colour-match a reference to indexed thumbnails. Supply semantic candidate IDs and visually judge the shortlist."""
    return service.profiles.reference_search(image_path, limit, candidates)


@mcp.tool()
def mapping_edit_history() -> dict:
    """List journalled patches and incomplete/failed attempts in the current map."""
    return service.vibe.history()


@mcp.tool()
def undo_mapping_edit(edit_id: str) -> dict:
    """Undo only one patch; refuses to overwrite later changes to its objects or locked elements."""
    return service.vibe.undo(edit_id)


@mcp.tool(structured_output=False)
def review_mapping(output_directory: str, center: list[float] | None = None, span: float | None = None) -> list:
    """Capture geometry/circulation checks and four views for an explicit visual review."""
    report = service.vibe.review(output_directory, center, span)
    return [report, *[Image(path=v["path"]) for v in report["captures"]["views"]]]


@mcp.tool()
def record_mapping_review(review_path: str, scores: dict[str, float], findings: list[dict], accepted: bool = False) -> dict:
    """Record image-grounded criterion scores and specific fixes. Refuses stale scene/project reviews."""
    return service.vibe.record_review(review_path, scores, findings, accepted)


@mcp.tool()
def mapping_environment(hour: int | None = None, minute: int | None = None,
                        weather_a: int | None = None, weather_b: int | None = None,
                        blend: float | None = None) -> dict:
    """Read available weather names or change only specified atmosphere fields; returns the previous state."""
    return service.environment(hour=hour, minute=minute, weather_a=weather_a, weather_b=weather_b, blend=blend)


@mcp.tool()
def suppress_native_models(x: float, y: float, radius: float, model_ids: list[int]) -> dict:
    """Temporarily hide an explicit native-model whitelist in a scratch session. Rollback restores it."""
    return service.suppress_native_models(x, y, radius, model_ids)


@mcp.tool()
def open_mapping_scene(logical_path: str, physical_path: str) -> dict:
    """Open an agent scratch IPL destination. Refuses to replace an active session."""
    if service.engine("session_status").get("active"):
        raise ValueError("finish the current scratch session before opening a different scene")
    return service.engine("scene", [logical_path, str(Path(physical_path).resolve())])


@mcp.tool()
def compare_mapping_variants(first: str, second: str) -> dict:
    """Compare two named checkpoints using durable keys and their saved creative brief."""
    return service.vibe.compare_variants(first, second)


@mcp.tool()
def export_mapping_review_bundle(output_directory: str) -> dict:
    """Write live scene/project/asset manifests and the last saved IPL, clearly distinguished."""
    return service.vibe.export_bundle(output_directory)


@mcp.tool()
def check_mapping_route(points: list[list[float]], width: float = 1.2, height: float = 1.8) -> dict:
    """Probe a ground-level xyz route against native and scratch collision, at foot/head height and lateral offsets."""
    return service.vibe.check_route(points, width, height)

@mcp.tool()
def selected_mapping_objects() -> dict:
    """Read the user's editor selection to ground instructions such as 'keep this' or 'move these'."""
    return service.engine("selection")

@mcp.tool()
def search_creative_catalogue(query: str, role: str | None = None, styles: list[str] | None = None,
                              avoid: list[str] | None = None, limit: int = 24,
                              max_dimensions: list[float] | None = None) -> dict:
    """Search French/English concepts. Hard role/size/exclusion filters, sourced observations ranked first."""
    return service.dispatch("catalogue.search", locals())


@mcp.tool()
def propose_asset_palette(brief: str, roles: list[str], styles: list[str] | None = None,
                          avoid: list[str] | None = None, per_role: int = 4,
                          max_dimensions: list[float] | None = None) -> dict:
    """Return coherent candidates grouped by role. Missing roles stay explicit; inspect a visual board before choosing."""
    return service.dispatch("catalogue.palette", locals())


@mcp.tool(structured_output=False)
def render_palette_board(asset_ids: list[int], output_directory: str, size: int = 192) -> list:
    """Render a labelled comparison board with two native views per asset and dimensions."""
    report = service.dispatch("catalogue.board", locals())
    return [report, Image(path=report["path"])]


@mcp.tool()
def propose_composition_variants(name: str, palette: dict[str, int], options: dict, variants: int = 3) -> dict:
    """Generate compact/balanced/open alternatives without changing the scene. Options include key,x,y,count."""
    return service.vibe.recipe_variants(name, palette, options, variants)


@mcp.tool()
def create_mapping_review_rig(capture_manifest: str, output_path: str) -> dict:
    """Freeze already reviewed camera poses for subsequent variant comparisons."""
    return service.dispatch("rig.create", locals())


@mcp.tool()
def capture_mapping_review_rig(rig_path: str, output_directory: str) -> dict:
    """Capture exactly the same views, preserving the live camera and recording atmosphere."""
    return service.dispatch("rig.capture", locals())


@mcp.tool(structured_output=False)
def compare_mapping_review_images(capture_paths: list[str], output_path: str) -> list:
    """Show two or three variants side by side; refuses differing camera rigs, atmosphere or scene."""
    report = service.dispatch("rig.compare", locals())
    return [report, Image(path=report["path"])]


@mcp.tool()
def inspect_placement_anchors(asset_id: int) -> dict:
    """Get measured bounds anchors and separately sourced functional anchors. Use names with place_relative."""
    return service.profiles.anchors(asset_id)


# ====================================================================== mapping toolkit
# SA-MP edition helpers (see MAPPING_TOOLKIT.md - call mapping_toolkit_guide first).
# Thin wrappers over mapping_tools / ground / lint / prefabs / building_tools /
# materials / custom_models so the CLI and the connector behave the same.

if __package__:
	from . import mapping_tools, ground as ground_mod, lint as lint_mod, prefabs as prefab_mod
	from . import building_tools, materials as material_mod, custom_models as custom_mod, blocks as blocks_mod
	from .ariane_ipc import ArianeClient
	from .asset_index import AssetIndex
else:
	import mapping_tools, ground as ground_mod, lint as lint_mod, prefabs as prefab_mod
	import building_tools, materials as material_mod, custom_models as custom_mod, blocks as blocks_mod
	from ariane_ipc import ArianeClient
	from asset_index import AssetIndex

TOOLKIT_GUIDE = Path(__file__).resolve().parent / "MAPPING_TOOLKIT.md"
_slow = None


def _slow_service():
	"""Same service with a long bridge timeout (scans, lint, settle over big areas)."""
	global _slow
	if _slow is None:
		_slow = ArianeService(
			Path(os.environ.get("ARIANE_ENGINE_SOCKET", DEFAULT_ENGINE_SOCKET)),
			Path(os.environ.get("ARIANE_ASSET_DB", DEFAULT_DATABASE)), 300.0,
			discovery_dir=Path(os.environ.get("ARIANE_DISCOVERY_DIR", DEFAULT_SESSION_DIR)),
			state_dir=Path(os.environ.get("ARIANE_AGENT_STATE_DIR", DEFAULT_STATE_DIR)))
	return _slow


def _bbox(bbox: list[float]) -> tuple[float, float, float, float]:
	if len(bbox) != 4:
		raise ValueError("bbox is [x0, y0, x1, y1]")
	x0, y0, x1, y1 = (float(v) for v in bbox)
	return min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1)


@mcp.tool()
def mapping_toolkit_guide() -> str:
	"""Read first: the SA-MP mapping workflow, every toolkit tool, site files, and hard-won rules (no wall gaps,
	nothing on tracks/roads, settle after placing, retexturing, custom models)."""
	return TOOLKIT_GUIDE.read_text(encoding="utf-8")


# ---------------------------------------------------------------- terrain
@mcp.tool()
def map_ground_scan(bbox: list[float], out_path: str, step: float = 1.0, png_path: str | None = None,
                    layer: Literal["ground", "top"] = "ground", include_agent: bool = False) -> dict:
	"""Raycast a grid over bbox [x0,y0,x1,y1]: ground height + collision surface per cell (DIRTTRACK, TARMAC,
	SAND...). Save it once per site; tracks, roads and structures are read from it by pack, facade and lint.
	'top' layer = first hit including roofs (structure mask). ~1 s per 2,500 cells."""
	from collections import Counter
	data = ground_mod.scan(_slow_service().client, _bbox(bbox), step, include_agent=include_agent)
	ground_mod.save(data, out_path)
	classes = Counter(ground_mod.class_at(data, i, layer) for i in range(data["nx"] * data["ny"]))
	result = {"ok": True, "out": out_path, "grid": [data["nx"], data["ny"]], "step": step,
	          "classes": dict(classes.most_common())}
	if png_path:
		ground_mod.render_png(data, png_path, layer)
		result["png"] = png_path
	return result


@mcp.tool()
def map_track_query(scan_path: str, points: list[list[float]], surfaces: list[str] | None = None) -> dict:
	"""Distance to and direction of the nearest track (default DIRTTRACK) for each [x, y]; use it to put gates on
	tracks, face buildings to the street or keep props off it. surfaces=['TARMAC',...] for roads."""
	data = ground_mod.load(scan_path)
	mask = ground_mod.TrackMask(data, tuple(surfaces or ["DIRTTRACK"]))
	out = []
	for x, y in points:
		d = mask.direction(x, y)
		out.append({"at": [x, y], "on_track": mask.on_track(x, y),
		            "distance": round(d[0], 2) if d else None,
		            "direction": [round(d[1], 3), round(d[2], 3)] if d else None,
		            "nearest": [round(d[3], 2), round(d[4], 2)] if d else None})
	return {"ok": True, **mask.stats(), "points": out}


@mcp.tool(structured_output=False)
def map_capture_ortho(cx: float, cy: float, half_width: float, out_path: str, cam_z: float = 250.0,
                      lod_mult: float = 3.0, delay_frames: int = 30) -> list:
	"""Straight-down orthographic screenshot centred on (cx, cy), half_width metres either side (known scale,
	no fog). The best way to check layouts, gaps and overlaps by eye. Ariane's window must not be minimised."""
	result = _slow_service().client.command("capture_ortho", str(Path(out_path).resolve()), cx, cy, half_width,
	                                        cam_z, lod_mult, delay_frames)
	return [result, Image(path=str(Path(out_path).resolve()))]


@mcp.tool(structured_output=False)
def map_plan(bbox: list[float], out_path: str, scan_path: str | None = None, issues_path: str | None = None,
             scale: float = 3.0) -> list:
	"""Top-down plan PNG: world buildings (grey), trees/poles (green dots), placed objects (blue, small props
	purple, ground planes outlined), tracks (orange) / roads (dark) from a scan, lint issues (red circles)."""
	result = mapping_tools.plan_map(_slow_service(), _bbox(bbox), out_path, scan=scan_path, issues=issues_path, scale=scale)
	return [result, Image(path=out_path)]


@mcp.tool()
def map_world_items(bbox: list[float], name_filter: str | None = None, limit: int = 400) -> dict:
	"""World (stock map) objects whose origin is inside bbox - buildings, trees, poles, fences - with model,
	position and heading. Use before removing/replacing buildings or to find free ground."""
	items = mapping_tools.world_items(_slow_service(), _bbox(bbox))
	if name_filter:
		items = [i for i in items if name_filter.lower() in str(i.get("name", "")).lower()]
	return {"ok": True, "count": len(items), "items": items[:limit], "truncated": len(items) > limit}


# ---------------------------------------------------------------- building blocks
@mcp.tool()
def map_settle(bbox: list[float] | None = None, models: list[int] | None = None, ids: list[int] | None = None,
               visual: bool = False, edge_stat: Literal["p25", "max"] = "p25", max_sink: float = 3.0,
               min_gap: float = 0.05, dry_run: bool = False) -> dict:
	"""Lower placed objects (with everything stacked on them) until they touch the terrain - run after every
	batch of placements; snapping alone leaves objects floating on slopes. visual=True grounds the drawn walls of
	buildings whose mesh sits above their collision (edge_stat 'max' for mounds/heaps/slabs)."""
	return mapping_tools.settle(_slow_service(), _slow_service().client, bbox=_bbox(bbox) if bbox else None,
	                            models=models, ids=ids, min_gap=min_gap, dry_run=dry_run, visual=visual,
	                            max_sink=max_sink, edge_stat=edge_stat)


@mcp.tool()
def map_delete_many(instance_ids: list[int]) -> dict:
	"""Delete many placed objects in a few round trips."""
	return mapping_tools.delete_objects(service, instance_ids)


@mcp.tool()
def map_transform_many(rows: list[dict]) -> dict:
	"""Move many placed objects: rows of {instance_id, x, y, z, heading[, snap]}."""
	return mapping_tools.transform_objects(service, rows)


@mcp.tool()
def map_wall_run(points: list[list[float]], closed: bool = True, gates: list[dict] | None = None,
                 containers: list[int] | None = None, container_levels: int = 2, fence: int | None = 19641,
                 mix: float = 0.65, seed: int = 1, gate_posts: bool = True, dry_run: bool = False) -> dict:
	"""Build a perimeter wall along a polygon/polyline with no gaps: stacked shipping containers mixed with fence
	sections laid end to end at their real length; slack is hidden as overlap behind containers; corners closed.
	gates: [{x, y, opening}] centred ON the wall line (and on a dirt track - check with map_track_query); each
	gate gets two container posts across the wall. Run with dry_run first; then map_settle and lint."""
	return mapping_tools.wall_run(service, points, closed=closed, gates=gates,
	                              containers=tuple(containers or (2932, 2934, 2935)), container_levels=container_levels,
	                              fence=fence, mix=mix, seed=seed, gate_posts=gate_posts, dry_run=dry_run)


@mcp.tool()
def map_pack(zone: list[list[float]], models: list[int] | None = None, count: int = 20,
             weights: list[float] | None = None, gap: float = 1.5, step: float = 2.0, scan_path: str | None = None,
             align: str = "track", seed: int = 1, dry_run: bool = False, style: str | None = None,
             style_role: str | None = None, style_mode: Literal["prefer", "limit"] = "prefer") -> dict:
	"""Fill a polygon zone with up to `count` structures on genuinely free ground: clear of world buildings,
	trees, placed objects (gap m), dirt tracks and roads (needs scan_path). Sites nearest the tracks first, facing
	them (align='track'; or 'random' / a heading). Good for shacks, tents, stalls; dense but tidy.
	style: without models, use that mapping style's models of style_role ('shelter', 'vehicle,debris'...) with
	their weights; with models, 'prefer' weights the style's up, 'limit' keeps only the style's."""
	return mapping_tools.pack(_slow_service(), zone, models, count=count, weights=weights, gap=gap, step=step,
	                          scan=scan_path, align=align, seed=seed, dry_run=dry_run, style=style,
	                          style_role=style_role, style_mode=style_mode)


# ---------------------------------------------------------------- mapping styles
if __package__:
	from . import styles as styles_mod
else:
	import styles as styles_mod


@mcp.tool()
def styles_list() -> dict:
	"""Mapping styles the user curated (wasteland, ...): name, description, models per role. When the user asks
	for a map 'in the X style', call style_show(X) first and choose models from it."""
	return {"ok": True, "dir": str(styles_mod.styles_dir()), "styles": styles_mod.list_styles(),
	        "roles": list(styles_mod.ROLES)}


@mcp.tool()
def style_show(style: str) -> dict:
	"""One style in full: models grouped by role (id, name, weight = how often, notes), preferred texture presets
	(material_preset names, resolved), the avoid list and the style's rules (notes). Use only these models for
	that style unless nothing fits a needed role; never use avoid-list models."""
	return styles_mod.show_style(style)


@mcp.tool()
def style_pick(style: str, role: str | None = None, count: int = 1, seed: int | None = None) -> dict:
	"""Weighted pick of `count` models from a style (role: 'shelter', 'wall,ruin', or None = any). Distinct
	models while the pool lasts, then repeats. Avoid-list models are never returned."""
	return styles_mod.pick(style, role, count, seed)


@mcp.tool()
def style_add_models(style: str, models: list[int], role: str | None = None, weight: float | None = None,
                     notes: str | None = None, description: str | None = None,
                     textures: list[str] | None = None, avoid: list[int] | None = None,
                     allow_hazards: bool = False) -> dict:
	"""Add (or update role / weight / notes of) models in a style; creates the style if needed. Every id is
	probed in the running editor (must exist) and explosive props are refused unless allow_hazards. textures
	adds texture preset names, avoid adds model ids the style must never use. The editor's Mapping Styles
	window shows the change."""
	result = styles_mod.add_models(style, models, role=role, weight=weight, notes=notes,
	                               description=description or "", service=service, allow_hazards=allow_hazards)
	if description is not None or textures or avoid:
		styles_mod.update_style(style, description=description, add_textures=textures or [], add_avoid=avoid or [])
	return result


@mcp.tool()
def style_remove_models(style: str, models: list[int]) -> dict:
	"""Remove models from a style (the style itself stays)."""
	return styles_mod.remove_models(style, models)


@mcp.tool()
def map_lint(site_path: str, max_issues: int = 40, issues_out: str | None = None,
             fix_ids_out: str | None = None, protect: list[int] | None = None) -> dict:
	"""Check a site against the mapping rules: wall/perimeter gaps, objects inside each other, clashes with world
	buildings and tree trunks, objects on dirt tracks or roads, props perched on fences/roofs. Site JSON format is
	in mapping_toolkit_guide. Fix every issue (or justify it) before syncing."""
	site_file = Path(site_path)
	site = json.loads(site_file.read_text(encoding="utf-8"))
	result = lint_mod.lint(_slow_service(), site, base_dir=site_file.parent)
	if fix_ids_out:
		kill, flagged = lint_mod.fix_ids(result, set(protect or []))
		Path(fix_ids_out).write_text(" ".join(map(str, kill)), encoding="utf-8")
		result["fix"] = {"delete": len(kill), "file": fix_ids_out, "protected_but_flagged": flagged}
	result.pop("_internal", None)
	if issues_out:
		Path(issues_out).write_text(json.dumps(result["issues"], indent=1), encoding="utf-8")
	issues = result.pop("issues")
	return {**result, "issues_total": len(issues), "issues": issues[:max_issues]}


# ---------------------------------------------------------------- prefabs
@mcp.tool()
def prefab_list() -> dict:
	"""Saved and bundled prefabs (multi-part structures: gatehouse, ruined lot...)."""
	return {"ok": True, "prefabs": prefab_mod.list_prefabs(service)}


@mcp.tool()
def prefab_save(name: str, instance_ids: list[int], origin: list[float] | None = None, notes: str = "",
                rigid: bool = False, origin_z: float | None = None) -> dict:
	"""Save placed objects as a reusable prefab (stacking, tilt and material overrides are kept). origin [x, y,
	heading] defaults to the first instance. rigid=True for objects built from building blocks: every part keeps
	its exact 3D offset (z from the ground under the origin, or origin_z) and the prefab is placed as one piece."""
	return prefab_mod.save_prefab(service, name, instance_ids, tuple(origin) if origin else None, notes,
	                              rigid=rigid, origin_z=origin_z)


@mcp.tool()
def prefab_place(name: str, x: float, y: float, heading: float, snap: bool = True, base_z: float | None = None) -> dict:
	"""Place a prefab; ground parts snap to terrain, stacked parts follow. base_z puts it on a slab/floor
	(snapping ignores placed objects). Run map_settle afterwards on slopes."""
	return prefab_mod.place_prefab(service, prefab_mod.load_prefab(service, name), x, y, heading, snap=snap, base_z=base_z)


# ---------------------------------------------------------------- world buildings
@mcp.tool()
def building_replace(x: float, y: float, model: int, old_model: int | None = None, heading_offset: float = 0.0,
                     dx: float = 0.0, dy: float = 0.0, dz: float | None = None, keep_old: bool = False,
                     wire_poles: bool = True) -> dict:
	"""Swap the world building at (x, y) (plus its LOD) for another model at the same transform - e.g. a house for
	a ruin. Fills the hole a removed lot leaves with ground planes and, with wire_poles, plants a pole at each
	power line left hanging over the old footprint. Check the result with map_capture_ortho."""
	return building_tools.replace_building(service, x, y, model, old_model=old_model, heading_offset=heading_offset,
	                                       dx=dx, dy=dy, dz=dz, keep_old=keep_old, wire_poles=wire_poles)


@mcp.tool()
def building_facade(x: float, y: float, model: int, side: str = "auto", spacing: float | None = None,
                    offset: float = 0.3, levels: str = "auto", level_height: float | None = None,
                    heading_offset: float = 0.0, max_levels: int = 6, scan_path: str | None = None,
                    building_model: int | None = None, min_run: int = 3, min_levels: int = 2) -> dict:
	"""Dress a real wall of the world building at (x, y) with `model` (scaffolding bays, boarding...). Probes the
	actual wall (two heights, rejects galleries/overhangs) instead of the bounding box; side 'auto' faces the road
	(give scan_path). Skips runs shorter than min_run bays."""
	mask = None
	if scan_path:
		data = ground_mod.load(scan_path)
		names = [n.upper() for n in data["surface_names"]]
		mask = ground_mod.TrackMask(data, tuple(n for n in names if ground_mod.surface_class(n) == "road"), min_cells=20)
	return building_tools.facade(service, x, y, model, side=side, spacing=spacing, offset=offset,
	                             levels=int(levels) if str(levels).isdigit() else "auto", level_height=level_height,
	                             heading_offset=heading_offset, max_levels=max_levels, road_mask=mask,
	                             building_model=building_model, min_run=min_run, min_levels=min_levels)


@mcp.tool()
def building_wire_ends(box: list[float], ground_z: float) -> dict:
	"""Power-line ends hanging over a rectangle [x0,y0,x1,y1] (e.g. after removing a building): plant a pole
	(1308) at each."""
	x0, y0, x1, y1 = _bbox(box)
	return {"ok": True, "ends": building_tools.wire_ends(service, [(x0, y0), (x1, y0), (x1, y1), (x0, y1)], ground_z)}


# ---------------------------------------------------------------- assets
@mcp.tool()
def asset_open_sides(model: int) -> dict:
	"""Which sides of a model are not drawn (open-backed rubble, facades): cover them or turn them to a wall."""
	a = service.probe_asset(model, ensure_renderable=True)
	areas = a.get("mesh_side_area") or {}
	opposite = {"+x": "-x", "-x": "+x", "+y": "-y", "-y": "+y"}
	open_sides = [k for k in opposite if areas and areas[k] < 0.15 * max(areas[opposite[k]], 1.0)]
	return {"ok": True, "model": model, "name": a.get("name"), "side_area_m2": areas, "open_sides": open_sides}


@mcp.tool()
def asset_size_match(width: float, depth: float, query: str = "", tolerance: float = 0.25, limit: int = 20,
                     include_hazards: bool = False) -> dict:
	"""Models with a similar footprint (e.g. a ruin to replace a building of width x depth metres). Explosive
	models are excluded unless include_hazards."""
	return {"assets": building_tools.size_match(AssetIndex(Path(os.environ.get("ARIANE_ASSET_DB", DEFAULT_DATABASE))),
	                                            width, depth, query=query, tolerance=tolerance, limit=limit,
	                                            include_hazards=include_hazards)}


@mcp.tool()
def asset_mark_hazards(game_dir: str | None = None) -> dict:
	"""Tag explosive / breakable / dynamic models from data/object.dat in the asset index (search then hides
	explosive ones). Run once per install."""
	game = game_dir or os.environ.get("ARIANE_GAME_DIR")
	if not game:
		raise ValueError("pass game_dir or set ARIANE_GAME_DIR")
	return AssetIndex(Path(os.environ.get("ARIANE_ASSET_DB", DEFAULT_DATABASE))).mark_hazards(game)


# ---------------------------------------------------------------- SA-MP materials
@mcp.tool()
def material_set(instance_id: int, index: int, model: int, txd: str, texture: str, color: str = "0") -> dict:
	"""SetDynamicObjectMaterial on a placed object: material slot `index` gets `texture` from txd `txd` (model =
	any model using that txd). color ARGB '0xFF6E5A46' tints (0 keeps); txd/texture 'none' keeps the texture and
	only recolours. Find slots with model_materials. Drawn in Ariane, exported and synced."""
	return material_mod.set_material(service, instance_id, index, model, txd, texture, color)


@mcp.tool()
def material_preset(instance_id: int, index: int, preset: str, color: str = "0") -> dict:
	"""Apply a named wasteland texture preset (rusty_metal, corrugated_metal, barn_wood, burnt_wall, concrete,
	invisible...; see texture_presets) to one material slot."""
	return material_mod.set_preset(service, instance_id, index, preset, color)


@mcp.tool()
def material_text(instance_id: int, index: int, text: str, size: int = 90, font: str = "Arial", font_size: int = 24,
                  bold: bool = True, font_color: str = "0xFFFFFFFF", back_color: str = "0", align: int = 1) -> dict:
	"""SetDynamicObjectMaterialText: signs, graffiti, notices. size = OBJECT_MATERIAL_SIZE_* (90 = 256x128); align
	0/1/2 = left/centre/right; newline with '\\n'. Ariane shows the slot as its background colour only."""
	return material_mod.set_material_text(service, instance_id, index, text, size=size, font=font, font_size=font_size,
	                                      bold=bold, font_color=font_color, back_color=back_color, align=align)


@mcp.tool()
def material_clear(instance_id: int, index: int = -1) -> dict:
	"""Remove material overrides (all slots, or one)."""
	return material_mod.clear_materials(service, instance_id, index)


@mcp.tool()
def material_get(instance_id: int) -> dict:
	"""Material overrides on a placed object."""
	return material_mod.get_materials(service, instance_id)


@mcp.tool()
def material_copy(from_id: int, to_ids: list[int]) -> dict:
	"""Copy every material override from one placed object to others (same model family)."""
	return {"ok": True, "copied": [material_mod.copy_materials(service, from_id, t)["instance_id"] for t in to_ids]}


@mcp.tool()
def model_materials(model: int) -> dict:
	"""A model's material slots (index -> current texture + colour), its txd and every texture in that txd."""
	return material_mod.model_materials(service, model)


@mcp.tool()
def txd_textures(txd: str) -> dict:
	"""Every texture in a txd (by name) - for picking retexture sources."""
	return material_mod.txd_textures(service, txd)


@mcp.tool()
def texture_presets(check: bool = False) -> dict:
	"""Named retexture presets (model, txd, texture); check=True verifies each resolves in this install."""
	out = {"ok": True, "presets": material_mod.TEXTURE_PRESETS}
	if check:
		out["resolves"] = material_mod.check_presets(service)
	return out


# ---------------------------------------------------------------- SA-MP custom models
@mcp.tool()
def custom_models_list() -> dict:
	"""Custom models from <server>/models/artconfig.txt: samp_id (negative, used in game) -> model_id (place this
	one in Ariane), base model, files, load errors."""
	return custom_mod.list_custom_models(service)


@mcp.tool()
def custom_models_reload(models_dir: str | None = None) -> dict:
	"""Re-read artconfig.txt (default <server>/models) after adding files."""
	return custom_mod.reload_custom_models(service, models_dir)


@mcp.tool()
def custom_model_add(server_dir: str, dff_path: str, txd_path: str, base_id: int, samp_id: int | None = None,
                     world: int = -1) -> dict:
	"""Install a custom model: copy dff/txd into <server>/models, append AddSimpleModel to artconfig.txt (next free
	id from -1000 down unless given) and load it. base_id = stock model whose collision/flags it uses in game."""
	return custom_mod.add_simple_model(service, server_dir, dff_path, txd_path, base_id=base_id, samp_id=samp_id,
	                                   world=world)


# ---------------------------------------------------------------- building blocks
Texture = str | list | None


def _tex(texture):
	"""preset name, or [model, txd, texture]"""
	return tuple(texture) if isinstance(texture, list) else texture


@mcp.tool()
def blocks_list(kind: str | None = None) -> dict:
	"""Building blocks: plain stock SA-MP shapes with collision and ONE material slot (retexture slot 0) - cube,
	block, beam, slab, panel, panel_door, panel_window, plate, sheet, pole - with exact sizes and pivots. Prefer the
	block_* builders, which pick and tile pieces for an exact size."""
	return {"ok": True, "blocks": blocks_mod.list_blocks(kind)}


@mcp.tool()
def block_place(block: str, x: float, y: float, z: float, heading: float = 0.0, pitch: float = 0.0, roll: float = 0.0,
                texture: Texture = None, color: str | None = None) -> dict:
	"""One block (model id, name or kind) with its box centred on (x, y, z). Rotation: heading about Z, pitch about
	local X, roll about local Y. texture: preset (white, grey, concrete, rusty_metal... see texture_presets) or
	[model, txd, texture]; color: ARGB tint, e.g. '0xFF8A6A4A' (blank presets take any colour)."""
	return {"ok": True, "instance_id": blocks_mod.place_block(service, block, x, y, z, heading=heading, pitch=pitch,
	                                                          roll=roll, texture=_tex(texture), color=color)}


@mcp.tool()
def block_box(x: float, y: float, z: float, width: float, depth: float, height: float, heading: float = 0.0,
              pitch: float = 0.0, roll: float = 0.0, texture: Texture = None, color: str | None = None,
              dry_run: bool = False) -> dict:
	"""Solid box of EXACT width (local X) x depth (local Y) x height (local Z) centred on (x, y, z), tiled from the
	fewest stock pieces that fit (no piece sticks out; the last of a row is pulled back flush). Floors, plinths,
	counters, crates, slabs, thick walls. Thinnest square section is 1 m - 'overshoot' reports when a piece is
	bigger than asked (use a panel/pole instead)."""
	return blocks_mod.build_box(service, x, y, z, width, depth, height, heading=heading, pitch=pitch, roll=roll,
	                            texture=_tex(texture), color=color, dry_run=dry_run)


@mcp.tool()
def block_panel(x: float, y: float, z: float, width: float, height: float, thickness: float = 0.18,
                heading: float = 0.0, pitch: float = 0.0, roll: float = 0.0, texture: Texture = None,
                color: str | None = None) -> dict:
	"""Flat plate width (local X) x height (local Z), thickness along local Y, centred on (x, y, z): walls, doors,
	signs, partitions; pitch=90 lays it flat (or use block_box)."""
	return blocks_mod.build_panel(service, x, y, z, width, height, thickness=thickness, heading=heading, pitch=pitch,
	                              roll=roll, texture=_tex(texture), color=color)


@mcp.tool()
def block_room(x: float, y: float, z_floor: float, width: float, depth: float, height: float, heading: float = 0.0,
               thickness: float = 0.18, floor: bool = True, ceiling: bool = False, door: dict | None = None,
               texture: Texture = None, color: str | None = None) -> dict:
	"""Hollow shell standing on z_floor: four walls (outer size), optional floor and ceiling.
	door = {"side": "-y"|"+y"|"-x"|"+x", "width": 1.4, "height": 2.2, "offset": 0} leaves an opening."""
	return blocks_mod.build_room(service, x, y, z_floor, width, depth, height, heading=heading, thickness=thickness,
	                             floor=floor, ceiling=ceiling, door=door, texture=_tex(texture), color=color)


@mcp.tool()
def block_beam(start: list[float], end: list[float], thickness: float = 0.25, width: float | None = None,
               texture: Texture = None, color: str | None = None) -> dict:
	"""Square beam along the centre line start [x,y,z] -> end: rafters, braces, frames (min section 0.18 m, panel
	on edge; 1 m blocks above that)."""
	return blocks_mod.build_beam(service, tuple(start), tuple(end), thickness=thickness, width=width,
	                             texture=_tex(texture), color=color)


@mcp.tool()
def block_pole(start: list[float], end: list[float], texture: Texture = None, color: str | None = None) -> dict:
	"""Steel pole start -> end: posts, railings, pipes. One piece is 10.9 m: a shorter pole ends at `end` and runs
	on past `start` (put start in the ground or inside a block). Its bottom 5 m is a 0.24 m square sleeve, the top
	5.9 m a thin 0.12 m shaft: up to 5.9 m only the shaft shows, longer ones show the sleeve as a footing collar."""
	return blocks_mod.build_pole(service, tuple(start), tuple(end), texture=_tex(texture), color=color)


@mcp.tool()
def block_ramp(low: list[float], high: list[float], width: float, thickness: float = 0.18, texture: Texture = None,
               color: str | None = None) -> dict:
	"""Sloped plate whose TOP surface runs from the low edge centre to the high edge centre; width across the
	slope. Ramps, stairs boards, lean-to roofs, awnings."""
	return blocks_mod.build_ramp(service, tuple(low), tuple(high), width, thickness=thickness, texture=_tex(texture),
	                             color=color)


@mcp.tool()
def block_roof(x: float, y: float, z_eave: float, length: float, span: float, rise: float, heading: float = 0.0,
               thickness: float = 0.18, overhang: float = 0.3, texture: Texture = None, color: str | None = None) -> dict:
	"""Gable roof over a length (local X) x span (local Y) rectangle centred on (x, y): two slopes from the eaves at
	z_eave to a ridge `rise` higher, overhanging by `overhang`. Gable ends stay open (add panels)."""
	return blocks_mod.build_roof(service, x, y, z_eave, length, span, rise, heading=heading, thickness=thickness,
	                             overhang=overhang, texture=_tex(texture), color=color)


@mcp.tool()
def block_column(x: float, y: float, z: float, radius: float, height: float, sides: int = 8, thickness: float = 0.18,
                 texture: Texture = None, color: str | None = None) -> dict:
	"""Round column: a ring of `sides` panels, outer radius `radius`, standing on z: silos, tanks, towers, pillars."""
	return blocks_mod.build_column(service, x, y, z, radius, height, sides=sides, thickness=thickness,
	                               texture=_tex(texture), color=color)


# ---------------------------------------------------------------- server
@mcp.tool()
def samp_export(out_path: str, format: Literal["streamer", "native", "wrapper", "current"] = "current",
                map_name: str | None = None, prefix: str | None = None) -> dict:
	"""SA-MP > Export Pawn: the layer (objects, removals, materials) as a Pawn include. streamer =
	CreateDynamicObject (streamer plugin); native = CreateObject (open.mp and SA-MP share these natives); wrapper =
	<prefix>CreateObject / RemoveBuilding / SetObjectMaterial functions defined by the user's script."""
	fmt = {"wrapper": 0, "streamer": 1, "native": 2, "current": -1}[format]
	return service.client.command("samp_export", str(Path(out_path).resolve()), fmt, map_name or "-", prefix or "-")


@mcp.tool()
def samp_sync(map_name: str | None = None, force: bool = False) -> dict:
	"""SA-MP > Sync to server: writes scriptfiles/ariane_live.txt (objects, removals, materials) which the running
	server reloads within seconds (tools/samp/ariane_live.pwn filterscript), plus the Pawn include if enabled in
	SA-MP > Server folder. Refuses an empty layer unless force."""
	return service.client.command("samp_sync", map_name or "-", int(force))


def main():
	mcp.run(transport="stdio")


if __name__ == "__main__":
	main()
