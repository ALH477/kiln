# SPDX-License-Identifier: MIT
@tool
extends EditorPlugin

## Kiln Map — the editor half of tools/mapmaker/tscn_map.py.
##
## This addon deliberately contains NO knowledge of the .map format. No
## winding table, no plane maths, and not one of the engine's limits written
## as a GDScript constant: the numbers come out of tools/schema/level_vocab.json
## at runtime, and the file itself is written by tscn_map.py.
##
## That is not fastidiousness. tools/schema/level_vocab.py's docstring records
## what happened the last time these facts lived in more than one place: six
## hand-maintained copies of the classname, epair, limit and winding tables,
## which had drifted, in a tree where an inside-out brush loads on console,
## collides correctly and draws nothing. A GDScript emitter here would be the
## seventh copy, in the one language no check in this repo runs.
##
## So the dock reads, counts and warns; `./dev map-from-tscn` decides.

const DOCK := preload("res://addons/kiln_map/dock.gd")

var _dock: Control


func _enter_tree() -> void:
	_dock = DOCK.new()
	_dock.name = "Kiln Map"
	add_control_to_dock(DOCK_SLOT_RIGHT_UL, _dock)
	# EditorInterface is a singleton from 4.2 on; get_editor_interface() is
	# deprecated there and gone later. Prefer the singleton, fall back, so the
	# addon loads on either vintage rather than erroring at _enter_tree.
	var ei = Engine.get_singleton("EditorInterface") if \
		Engine.has_singleton("EditorInterface") else get_editor_interface()
	_dock.bind_editor(ei)


func _exit_tree() -> void:
	if _dock:
		remove_control_from_docks(_dock)
		_dock.queue_free()
		_dock = null
