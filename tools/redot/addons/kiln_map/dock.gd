# SPDX-License-Identifier: MIT
@tool
extends VBoxContainer

## The budget dock: what this level costs, against what the engine will take.
##
## Every ceiling here is one kiln_map.c enforces by DROPPING things. Past
## MAX_BRUSHES the brushes past it are not loaded; past MAX_SPAWNS the spawns
## are not spawned; past the coord range a face vertex wraps, because they are
## packed int16. All of it is a debugf on a console nobody is watching, so the
## level simply comes back missing a wall and the author has no reason to
## suspect a limit. The whole point of this panel is that you are told at the
## moment you cross one, with the node named.
##
## The numbers are read from tools/schema/level_vocab.json (plus any
## KILN_LEVEL_VOCAB_OVERLAY the game sets) — never written here. See plugin.gd.
##
## This panel is an EARLY WARNING, not the gate. It counts nodes and measures
## extents, which is exact; it does not run the brush CSG, so it cannot tell
## you that a convex brush will exceed MAX_BRUSH_PLANES or that a face will
## exceed MAX_FACE_VERTS. `./dev map-from-tscn` runs the real solver and
## refuses the level if it does not fit — press Export, or Check, and read it.

const META_TEX := "kiln_texture"
const META_CLASS := "kiln_classname"
const META_EPAIR := "kiln_epair_"
const META_WORLD := "kiln_worldspawn_"

## Units per authored metre. Matches tscn_map.py's --scale default and
## gltf_to_t3d --base-scale=64: this engine runs at 64 units to the metre.
const SCALE := 64.0

## Within this of a signed permutation, a basis is treated as axis-aligned.
const AXIS_EPS := 1e-4

var _editor: EditorInterface
var _vocab: Dictionary = {}
var _limits: Dictionary = {}
var _classnames: Array = []

var _repo_edit: LineEdit
var _rows: Dictionary = {}
var _warn_label: RichTextLabel
var _status: Label
var _grid: GridContainer


func _init() -> void:
	_build_ui()


func bind_editor(ei: EditorInterface) -> void:
	_editor = ei
	_load_vocab()
	_refresh()


# ── UI ──────────────────────────────────────────────────────────────────────

func _build_ui() -> void:
	custom_minimum_size = Vector2(260, 0)
	add_theme_constant_override("separation", 6)

	var title := Label.new()
	title.text = "Kiln budget"
	title.add_theme_font_size_override("font_size", 16)
	add_child(title)

	_grid = GridContainer.new()
	_grid.columns = 3
	add_child(_grid)

	var sep := HSeparator.new()
	add_child(sep)

	_warn_label = RichTextLabel.new()
	_warn_label.bbcode_enabled = true
	_warn_label.fit_content = true
	_warn_label.custom_minimum_size = Vector2(0, 90)
	add_child(_warn_label)

	var repo_row := HBoxContainer.new()
	var repo_lbl := Label.new()
	repo_lbl.text = "Kiln repo"
	repo_row.add_child(repo_lbl)
	_repo_edit = LineEdit.new()
	_repo_edit.placeholder_text = "/home/you/Documents/M64"
	_repo_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_repo_edit.text = _guess_repo()
	repo_row.add_child(_repo_edit)
	add_child(repo_row)

	var buttons := HBoxContainer.new()
	var refresh_btn := Button.new()
	refresh_btn.text = "Refresh"
	refresh_btn.pressed.connect(_refresh)
	buttons.add_child(refresh_btn)

	var check_btn := Button.new()
	check_btn.text = "Check"
	check_btn.tooltip_text = ("Run the real converter and validator over this "
		+ "scene without writing a .map.")
	check_btn.pressed.connect(func(): _run_converter(""))
	buttons.add_child(check_btn)

	var export_btn := Button.new()
	export_btn.text = "Export .map"
	export_btn.pressed.connect(_export)
	buttons.add_child(export_btn)
	add_child(buttons)

	var adders := HBoxContainer.new()
	var brush_btn := Button.new()
	brush_btn.text = "+ Brush"
	brush_btn.pressed.connect(_add_brush)
	adders.add_child(brush_btn)
	var spawn_btn := Button.new()
	spawn_btn.text = "+ Spawn"
	spawn_btn.pressed.connect(_add_spawn)
	adders.add_child(spawn_btn)
	add_child(adders)

	_status = Label.new()
	_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_status)


func _row(key: String, label: String) -> void:
	var name_lbl := Label.new()
	name_lbl.text = label
	var val_lbl := Label.new()
	val_lbl.text = "0"
	val_lbl.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	var cap_lbl := Label.new()
	cap_lbl.text = "/ ?"
	_grid.add_child(name_lbl)
	_grid.add_child(val_lbl)
	_grid.add_child(cap_lbl)
	_rows[key] = {"value": val_lbl, "cap": cap_lbl}


# ── the vocabulary, read never written ──────────────────────────────────────

func _guess_repo() -> String:
	# The addon normally lives at <kiln>/tools/redot/addons/kiln_map, and the
	# project is usually the game's. Offer the former as the default and let
	# the author correct it; nothing here hardcodes a path.
	var here := ProjectSettings.globalize_path("res://addons/kiln_map")
	var parts := here.split("/tools/redot/")
	if parts.size() == 2:
		return parts[0]
	return ""


func _vocab_path() -> String:
	var repo := _repo_edit.text.strip_edges()
	if repo == "":
		return ""
	return repo.path_join("tools/schema/level_vocab.json")


func _load_vocab() -> void:
	_vocab = {}
	_limits = {}
	_classnames = []
	var path := _vocab_path()
	if path == "" or not FileAccess.file_exists(path):
		_status.text = ("level_vocab.json not found. Set the Kiln repo path "
			+ "above — the limits and the classname palette are read from it, "
			+ "never stored here.")
		return
	var parsed = JSON.parse_string(FileAccess.get_file_as_string(path))
	if typeof(parsed) != TYPE_DICTIONARY:
		_status.text = "level_vocab.json did not parse."
		return
	_vocab = parsed
	_limits = _vocab.get("limits", {})
	# faces is DERIVED as brushes x 6, exactly as kiln_map.c derives MAX_FACES.
	_limits["faces"] = int(_limits.get("brushes", 0)) * 6
	for c in _vocab.get("classnames", []):
		_classnames.append(c)
	_load_overlay()

	if _rows.is_empty():
		_row("brushes", "brushes")
		_row("faces", "faces")
		_row("spawns", "spawns")
		_row("classnames", "classnames")
		_row("coord", "furthest")
	for key in _rows:
		_rows[key]["cap"].text = "/ %d" % int(_limits.get(key, 0))


func _load_overlay() -> void:
	# A game's own classnames (PetaByte-Madness' info_imp and friends) live in
	# that repo, not in the engine's schema. Same contract as level_vocab.py's
	# KILN_LEVEL_VOCAB_OVERLAY: classnames only.
	var env := OS.get_environment("KILN_LEVEL_VOCAB_OVERLAY")
	if env == "":
		return
	for path in env.split(":", false):
		if not FileAccess.file_exists(path):
			continue
		var add = JSON.parse_string(FileAccess.get_file_as_string(path))
		if typeof(add) != TYPE_DICTIONARY:
			continue
		for c in add.get("classnames", []):
			var replaced := false
			for i in _classnames.size():
				if _classnames[i].get("name", "") == c.get("name", ""):
					_classnames[i] = c
					replaced = true
					break
			if not replaced:
				_classnames.append(c)


func classname_list() -> Array:
	var out: Array = []
	for c in _classnames:
		out.append(c.get("name", ""))
	out.sort()
	return out


# ── counting what the scene costs ───────────────────────────────────────────

func _is_brush(n: Node) -> bool:
	if n is CSGBox3D:
		return true
	var mi := n as MeshInstance3D
	if mi != null and mi.mesh is BoxMesh:
		return true
	var cs := n as CollisionShape3D
	if cs != null and (cs.shape is ConvexPolygonShape3D or cs.shape is BoxShape3D):
		return true
	return false


func _is_axis_aligned(b: Basis) -> bool:
	# A signed permutation: each column on an axis. 90-degree turns and
	# negative scales pass; anything else is a brush that draws its true shape
	# and COLLIDES AS ITS BOUNDING BOX, because FigBrush is mins/maxs only.
	for col in [b.x, b.y, b.z]:
		var c := Vector3(absf(col.x), absf(col.y), absf(col.z))
		var on_axis := 0
		for v in [c.x, c.y, c.z]:
			if v > AXIS_EPS:
				on_axis += 1
		if on_axis != 1:
			return false
	return true


func _walk(n: Node, out: Dictionary) -> void:
	# `n3`, explicitly typed, rather than relying on the `is Node3D` narrowing:
	# GDScript does not infer a member's type through it, so `var o :=
	# n.global_transform.origin` is a PARSE error and the whole addon fails to
	# load. That is not a warning in the editor -- the dock simply never
	# appears, which reads as "the plugin is broken" with no clue why.
	var n3 := n as Node3D
	if n3 != null:
		var xf: Transform3D = n3.global_transform
		var meta_class := ""
		if n3.has_meta(META_CLASS):
			meta_class = str(n3.get_meta(META_CLASS))
		if meta_class != "":
			out["spawns"].append(n3)
			if not out["classnames"].has(meta_class):
				out["classnames"].append(meta_class)
			if not classname_list().has(meta_class) and _classnames.size() > 0:
				out["warn"].append("%s: classname \"%s\" is not in the vocabulary"
					% [n3.name, meta_class])
			var ep := 0
			for k in n3.get_meta_list():
				if str(k).begins_with(META_EPAIR):
					ep += 1
			if ep + 3 > 16:
				out["warn"].append("%s: %d epairs; a FigDict holds 16 keys "
					% [n3.name, ep] + "including classname, origin and angle")
		elif _is_brush(n3):
			out["brushes"].append(n3)
			if not _is_axis_aligned(xf.basis):
				out["aabb_only"].append(n3.name)
		var o: Vector3 = xf.origin * SCALE
		var far: float = maxf(maxf(absf(o.x), absf(o.y)), absf(o.z))
		out["coord"] = maxf(out["coord"], far)
	for c in n.get_children():
		_walk(c, out)


func _scan() -> Dictionary:
	var out := {"brushes": [], "spawns": [], "classnames": [],
		"aabb_only": [], "warn": [], "coord": 0.0}
	if _editor == null:
		return out
	var root := _editor.get_edited_scene_root()
	if root:
		_walk(root, out)
	return out


func _set_row(key: String, value: int) -> void:
	if not _rows.has(key):
		return
	var cap := int(_limits.get(key, 0))
	var lbl: Label = _rows[key]["value"]
	lbl.text = str(value)
	if cap > 0 and value > cap:
		lbl.add_theme_color_override("font_color", Color(1, 0.35, 0.35))
	elif cap > 0 and value > cap * 0.9:
		lbl.add_theme_color_override("font_color", Color(1, 0.8, 0.3))
	else:
		lbl.remove_theme_color_override("font_color")


func _refresh() -> void:
	if _limits.is_empty():
		_load_vocab()
	var s := _scan()
	_set_row("brushes", s["brushes"].size())
	_set_row("faces", s["brushes"].size() * 6)
	_set_row("spawns", s["spawns"].size())
	_set_row("classnames", s["classnames"].size())
	_set_row("coord", int(s["coord"]))

	var lines: Array = []
	for w in s["warn"]:
		lines.append("[color=#ff6666]%s[/color]" % w)
	if s["aabb_only"].size() > 0:
		lines.append(("[color=#ffcc66]%d brush(es) are not axis-aligned: "
			+ "they DRAW their true shape and BLOCK their bounding box. "
			+ "Keep anything walkable square. (%s)[/color]")
			% [s["aabb_only"].size(), ", ".join(s["aabb_only"].slice(0, 4))])
	if lines.is_empty():
		lines.append("[color=#88cc88]Within budget.[/color]")
	lines.append("[i]Counts are exact; the brush CSG is not run here. "
		+ "Export or Check for the authoritative answer.[/i]")
	_warn_label.text = "\n".join(lines)


# ── authoring helpers ───────────────────────────────────────────────────────

func _parent_for_new() -> Node:
	if _editor == null:
		return null
	var sel := _editor.get_selection().get_selected_nodes()
	if sel.size() > 0 and sel[0] is Node3D:
		return sel[0]
	return _editor.get_edited_scene_root()


func _attach(n: Node) -> void:
	var parent := _parent_for_new()
	if parent == null:
		_status.text = "Open a 3D scene first."
		return
	parent.add_child(n)
	n.owner = _editor.get_edited_scene_root()
	_editor.get_selection().clear()
	_editor.get_selection().add_node(n)
	_refresh()


func _add_brush() -> void:
	var b := CSGBox3D.new()
	b.name = "Brush"
	# Whole metres: at 64 units to the metre this lands on integers, and a
	# 0.125 m grid snap keeps every later edit there too.
	b.size = Vector3(1, 1, 1)
	b.set_meta(META_TEX, "DECK")
	_attach(b)


func _add_spawn() -> void:
	var m := Marker3D.new()
	var names := classname_list()
	m.name = "Spawn"
	m.set_meta(META_CLASS, names[0] if names.size() > 0 else "info_player_start")
	_attach(m)


# ── the authority: shell out to the converter ───────────────────────────────

func _scene_path() -> String:
	if _editor == null:
		return ""
	var root := _editor.get_edited_scene_root()
	if root == null:
		return ""
	return ProjectSettings.globalize_path(root.scene_file_path)


func _run_converter(out_path: String) -> void:
	var repo := _repo_edit.text.strip_edges()
	var scene := _scene_path()
	if repo == "" or scene == "":
		_status.text = "Save the scene and set the Kiln repo path first."
		return
	# Save first: the converter reads the FILE, and an unsaved edit is exactly
	# the difference the author will not think to look for.
	_editor.save_scene()

	var dest := out_path
	if dest == "":
		dest = "-"
	var args := [repo.path_join("tools/mapmaker/tscn_map.py"),
		"--to-map", scene, "--out", dest]
	var output: Array = []
	var code := OS.execute("python3", args, output, true)
	var text := ""
	for chunk in output:
		text += str(chunk)
	_status.text = text.strip_edges()
	if code == 0 and out_path != "":
		_status.text = "Wrote %s\n%s" % [out_path, _status.text]
	_refresh()


func _export() -> void:
	var scene := _scene_path()
	if scene == "":
		_status.text = "Save the scene first."
		return
	var dlg := EditorFileDialog.new()
	dlg.file_mode = EditorFileDialog.FILE_MODE_SAVE_FILE
	dlg.access = EditorFileDialog.ACCESS_FILESYSTEM
	dlg.add_filter("*.map", "Quake map")
	dlg.current_file = scene.get_file().get_basename() + ".map"
	add_child(dlg)
	dlg.file_selected.connect(func(p: String):
		_run_converter(p)
		dlg.queue_free())
	dlg.popup_centered_ratio(0.6)
