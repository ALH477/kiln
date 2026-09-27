/* SPDX-License-Identifier: MIT
 *
 * engine/src/kiln/kiln_compat.h — the old prefix, for one migration train.
 *
 * docs/NAMING.md section 9 step 2: "Add `fig_*` as the real symbols; `kiln_*`
 * becomes a compatibility macro in one header." This is that header, and it is
 * the ONLY place the old prefix is allowed to appear as an identifier.
 *
 * ── Why a macro and not a second set of declarations ───────────────────
 * Section 10 forbids a public header that ships both spellings as
 * first-class. A macro cannot be linked against, cannot appear in a symbol
 * table, and cannot drift from what it points at — if `fig_scene_begin` is
 * renamed again, every line here fails to compile rather than resolving to
 * something stale. Two sets of prototypes would do the opposite.
 *
 * ── How a downstream gets it ──────────────────────────────────────────
 * It is force-included by engine/kiln-inst.mk (`-include kiln_compat.h`), so
 * a game that has not migrated compiles unedited. That is one line in the
 * forge and one file here, which is the whole cost of the train, and deleting
 * both is the whole cost of ending it.
 *
 * ── What is NOT here, on purpose ──────────────────────────────────────
 *  - KILN_DEBUG and KILN_SRC. Those are the FORGE's build knobs, set by Nix
 *    and make, not engine symbols (section 4: the Kiln CLI / Nix layer keeps
 *    `kiln`). They are not renamed and so need no bridge.
 *  - Include guards. Nothing outside a header reads one.
 *  - Anything the game names that turned out to be a FILE (kiln_actor.h) or a
 *    forge asset (kiln_logo.t3dm). Files do not move in this step.
 *
 * ── The stop condition ────────────────────────────────────────────────
 * Section 9 step 6: delete this file when the tree and the Exsecutor-emitted
 * C no longer mention `kiln_*`. Until then, every entry below is a debt this
 * file makes countable — 277 of them, generated from what PetaByte-Madness
 * actually calls rather than from the whole engine surface, so the number
 * only goes down.
 */
#ifndef FIG_COMPAT_H
#define FIG_COMPAT_H

#define KILN_ACTOR_CAT_BOSS           FIG_ACTOR_CAT_BOSS
#define KILN_ACTOR_CAT_ENEMY          FIG_ACTOR_CAT_ENEMY
#define KILN_ACTOR_CAT_NPC            FIG_ACTOR_CAT_NPC
#define KILN_ACTOR_CAT_PLAYER         FIG_ACTOR_CAT_PLAYER
#define KILN_ACTOR_CAT_PROP           FIG_ACTOR_CAT_PROP
#define KILN_ACTOR_HANDLE_NONE        FIG_ACTOR_HANDLE_NONE
#define KILN_ACTOR_ROOM_NONE          FIG_ACTOR_ROOM_NONE
#define KILN_ACTOR_STATE_MAX          FIG_ACTOR_STATE_MAX
#define KILN_BTN_                     FIG_BTN_
#define KILN_BTN_A                    FIG_BTN_A
#define KILN_BTN_B                    FIG_BTN_B
#define KILN_BTN_CD                   FIG_BTN_CD
#define KILN_BTN_CL                   FIG_BTN_CL
#define KILN_BTN_CR                   FIG_BTN_CR
#define KILN_BTN_CU                   FIG_BTN_CU
#define KILN_BTN_DD                   FIG_BTN_DD
#define KILN_BTN_DL                   FIG_BTN_DL
#define KILN_BTN_DR                   FIG_BTN_DR
#define KILN_BTN_DU                   FIG_BTN_DU
#define KILN_BTN_L                    FIG_BTN_L
#define KILN_BTN_R                    FIG_BTN_R
#define KILN_BTN_START                FIG_BTN_START
#define KILN_BTN_Z                    FIG_BTN_Z
#define KILN_CAMLINT_ERR_COUNT        FIG_CAMLINT_ERR_COUNT
#define KILN_CAMLINT_ERR_DEGENERATE   FIG_CAMLINT_ERR_DEGENERATE
#define KILN_CAMLINT_ERR_FRUSTUM      FIG_CAMLINT_ERR_FRUSTUM
#define KILN_CAMLINT_ERR_KEY_PAST_END FIG_CAMLINT_ERR_KEY_PAST_END
#define KILN_CAMLINT_ERR_NAN          FIG_CAMLINT_ERR_NAN
#define KILN_CAMLINT_ERR_NO_KEYS      FIG_CAMLINT_ERR_NO_KEYS
#define KILN_CAMLINT_ERR_SUBJECT_CUT  FIG_CAMLINT_ERR_SUBJECT_CUT
#define KILN_CAMLINT_ERR_TIME_ORDER   FIG_CAMLINT_ERR_TIME_ORDER
#define KILN_CAMLINT_HITCH_RATIO      FIG_CAMLINT_HITCH_RATIO
#define KILN_CAMLINT_NOTE_DEAD_TAIL   FIG_CAMLINT_NOTE_DEAD_TAIL
#define KILN_CAMLINT_NOTE_HITCH       FIG_CAMLINT_NOTE_HITCH
#define KILN_CAMLINT_NOTE_LATE_START  FIG_CAMLINT_NOTE_LATE_START
#define KILN_CAMLINT_NOTE_NEAR_AIM    FIG_CAMLINT_NOTE_NEAR_AIM
#define KILN_CAMLINT_NOTE_OUTSIDE     FIG_CAMLINT_NOTE_OUTSIDE
#define KILN_CAMLINT_NOTE_OVERSHOOT   FIG_CAMLINT_NOTE_OVERSHOOT
#define KILN_CAMLINT_OVERSHOOT_FRAC   FIG_CAMLINT_OVERSHOOT_FRAC
#define KILN_CAMLINT_SAMPLES_PER_SEG  FIG_CAMLINT_SAMPLES_PER_SEG
#define KILN_CAMLINT_TAIL_FRAC        FIG_CAMLINT_TAIL_FRAC
#define KILN_CAM_CUTSCENE             FIG_CAM_CUTSCENE
#define KILN_CTX_NONE                 FIG_CTX_NONE
#define KILN_CTX_USE                  FIG_CTX_USE
#define KILN_PROJ_GRENADE             FIG_PROJ_GRENADE
#define KILN_PROJ_GRENADE_W           FIG_PROJ_GRENADE_W
#define KILN_PROJ_PLASMA              FIG_PROJ_PLASMA
#define KILN_PROJ_PLASMA_W            FIG_PROJ_PLASMA_W
#define KILN_PROJ_ROCKET              FIG_PROJ_ROCKET
#define KILN_PROJ_ROCKET_W            FIG_PROJ_ROCKET_W
#define KILN_SCENE_MAX_LIGHTS         FIG_SCENE_MAX_LIGHTS
#define KILN_TEXANIM_SCROLL           FIG_TEXANIM_SCROLL
#define KILN_TILE_MAX_LOD             FIG_TILE_MAX_LOD
#define KILN_WIDGET_CHAR_W            FIG_WIDGET_CHAR_W
#define KILN_WTYPE_HITSCAN            FIG_WTYPE_HITSCAN
#define KILN_WTYPE_PROJECTILE         FIG_WTYPE_PROJECTILE
#define KilnActor                     FigActor
#define KilnActorDrawFn               FigActorDrawFn
#define KilnActorHandle               FigActorHandle
#define KilnActorInitFn               FigActorInitFn
#define KilnActorProfile              FigActorProfile
#define KilnAsset                     FigAsset
#define KilnAudioConfig               FigAudioConfig
#define KilnBrush                     FigBrush
#define KilnCamBounds                 FigCamBounds
#define KilnCamKey                    FigCamKey
#define KilnCamReport                 FigCamReport
#define KilnCamShot                   FigCamShot
#define KilnCamera                    FigCamera
#define KilnContextAction             FigContextAction
#define KilnCraterField               FigCraterField
#define KilnDetailPlace               FigDetailPlace
#define KilnDetailStats               FigDetailStats
#define KilnDialogue                  FigDialogue
#define KilnDict                      FigDict
#define KilnFpsCam                    FigFpsCam
#define KilnInput                     FigInput
#define KilnLODConfig                 FigLODConfig
#define KilnMap                       FigMap
#define KilnMenu                      FigMenu
#define KilnProjHitFn                 FigProjHitFn
#define KilnProjType                  FigProjType
#define KilnRoomSpawn                 FigRoomSpawn
#define KilnScene                     FigScene
#define KilnSkel                      FigSkel
#define KilnSurfaceDef                FigSurfaceDef
#define KilnTexAnim                   FigTexAnim
#define KilnTrace                     FigTrace
#define KilnTransform                 FigTransform
#define KilnVideo                     FigVideo
#define KilnWeapon                    FigWeapon
#define KilnWeaponDef                 FigWeaponDef
#define KilnWeaponProj                FigWeaponProj
#define KilnWeaponSet                 FigWeaponSet
#define KilnWidgetStyle               FigWidgetStyle
#define kiln_actor                    fig_actor
#define kiln_actor_despawn            fig_actor_despawn
#define kiln_actor_draw_all           fig_actor_draw_all
#define kiln_actor_first              fig_actor_first
#define kiln_actor_handle_of          fig_actor_handle_of
#define kiln_actor_next               fig_actor_next
#define kiln_actor_resolve            fig_actor_resolve
#define kiln_actor_spawn              fig_actor_spawn
#define kiln_actor_spawn_in_room      fig_actor_spawn_in_room
#define kiln_actor_system_init        fig_actor_system_init
#define kiln_actor_update_all         fig_actor_update_all
#define kiln_asset                    fig_asset
#define kiln_asset_arena_used         fig_asset_arena_used
#define kiln_asset_close              fig_asset_close
#define kiln_asset_count              fig_asset_count
#define kiln_asset_open               fig_asset_open
#define kiln_asset_probe_size         fig_asset_probe_size
#define kiln_audio                    fig_audio
#define kiln_audio_init               fig_audio_init
#define kiln_audio_set_insert         fig_audio_set_insert
#define kiln_audio_update             fig_audio_update
#define kiln_camera                   fig_camera
#define kiln_camera_apply             fig_camera_apply
#define kiln_camera_init              fig_camera_init
#define kiln_camera_pop               fig_camera_pop
#define kiln_camera_push              fig_camera_push
#define kiln_camera_set_cutscene      fig_camera_set_cutscene
#define kiln_camera_snap              fig_camera_snap
#define kiln_camera_update            fig_camera_update
#define kiln_camkey                   fig_camkey
#define kiln_camkey_sample            fig_camkey_sample
#define kiln_camkey_spline1           fig_camkey_spline1
#define kiln_camlint                  fig_camlint
#define kiln_camlint_err_name         fig_camlint_err_name
#define kiln_camlint_note_name        fig_camlint_note_name
#define kiln_clip                     fig_clip
#define kiln_clip_ray                 fig_clip_ray
#define kiln_clip_set_world           fig_clip_set_world
#define kiln_clip_slide               fig_clip_slide
#define kiln_clip_world_count         fig_clip_world_count
#define kiln_console                  fig_console
#define kiln_context_scan             fig_context_scan
#define kiln_crater_destroy           fig_crater_destroy
#define kiln_crater_impact            fig_crater_impact
#define kiln_crater_init              fig_crater_init
#define kiln_crater_update            fig_crater_update
#define kiln_cull_surface_dist_sq     fig_cull_surface_dist_sq
#define kiln_cull_tier_of             fig_cull_tier_of
#define kiln_dd_aabb                  fig_dd_aabb
#define kiln_dd_axes                  fig_dd_axes
#define kiln_dd_begin                 fig_dd_begin
#define kiln_dd_box                   fig_dd_box
#define kiln_dd_clipped               fig_dd_clipped
#define kiln_dd_drawn                 fig_dd_drawn
#define kiln_dd_end                   fig_dd_end
#define kiln_dd_frustum               fig_dd_frustum
#define kiln_dd_line                  fig_dd_line
#define kiln_dd_path                  fig_dd_path
#define kiln_dd_point                 fig_dd_point
#define kiln_dd_text                  fig_dd_text
#define kiln_debugdraw                fig_debugdraw
#define kiln_detail_admit             fig_detail_admit
#define kiln_detail_draw              fig_detail_draw
#define kiln_detail_frame_begin       fig_detail_frame_begin
#define kiln_detail_model_visible     fig_detail_model_visible
#define kiln_detail_object            fig_detail_object
#define kiln_detail_stats             fig_detail_stats
#define kiln_dfs_exists               fig_dfs_exists
#define kiln_dialogue_active          fig_dialogue_active
#define kiln_dialogue_draw            fig_dialogue_draw
#define kiln_dialogue_start           fig_dialogue_start
#define kiln_dialogue_update          fig_dialogue_update
#define kiln_engine                   fig_engine
#define kiln_engine_init              fig_engine_init
#define kiln_event_init               fig_event_init
#define kiln_event_post               fig_event_post
#define kiln_event_process            fig_event_process
#define kiln_fpscam                   fig_fpscam
#define kiln_fpscam_apply             fig_fpscam_apply
#define kiln_fpscam_forward           fig_fpscam_forward
#define kiln_fpscam_init              fig_fpscam_init
#define kiln_fpscam_right             fig_fpscam_right
#define kiln_fpscam_snap              fig_fpscam_snap
#define kiln_fpscam_update            fig_fpscam_update
#define kiln_frame_begin              fig_frame_begin
#define kiln_frame_end                fig_frame_end
#define kiln_gui                      fig_gui
#define kiln_gui_bar                  fig_gui_bar
#define kiln_gui_begin                fig_gui_begin
#define kiln_gui_end                  fig_gui_end
#define kiln_gui_line                 fig_gui_line
#define kiln_gui_panel                fig_gui_panel
#define kiln_gui_rect                 fig_gui_rect
#define kiln_gui_text                 fig_gui_text
#define kiln_input                    fig_input
#define kiln_input_get                fig_input_get
#define kiln_input_init               fig_input_init
#define kiln_input_update             fig_input_update
#define kiln_lod                      fig_lod
#define kiln_lod_select               fig_lod_select
#define kiln_map                      fig_map
#define kiln_map_draw                 fig_map_draw
#define kiln_map_load                 fig_map_load
#define kiln_map_register_classname   fig_map_register_classname
#define kiln_menu_draw                fig_menu_draw
#define kiln_menu_init                fig_menu_init
#define kiln_menu_move                fig_menu_move
#define kiln_music_load               fig_music_load
#define kiln_music_play               fig_music_play
#define kiln_music_playing            fig_music_playing
#define kiln_music_set_loop           fig_music_set_loop
#define kiln_music_set_volume         fig_music_set_volume
#define kiln_music_stop               fig_music_stop
#define kiln_panic_message            fig_panic_message
#define kiln_projectile_draw_all      fig_projectile_draw_all
#define kiln_projectile_init          fig_projectile_init
#define kiln_projectile_set_hit_fn    fig_projectile_set_hit_fn
#define kiln_projectile_spawn         fig_projectile_spawn
#define kiln_projectile_update        fig_projectile_update
#define kiln_room_system_update       fig_room_system_update
#define kiln_save_erase               fig_save_erase
#define kiln_save_exists              fig_save_exists
#define kiln_save_init                fig_save_init
#define kiln_save_read                fig_save_read
#define kiln_save_write               fig_save_write
#define kiln_scene_begin              fig_scene_begin
#define kiln_scene_disable_fog        fig_scene_disable_fog
#define kiln_scene_frustum            fig_scene_frustum
#define kiln_scene_init               fig_scene_init
#define kiln_scene_project            fig_scene_project
#define kiln_scene_set_fog            fig_scene_set_fog
#define kiln_scene_update             fig_scene_update
#define kiln_sdbfs                    fig_sdbfs
#define kiln_sdbfs_key_is_dma         fig_sdbfs_key_is_dma
#define kiln_sdbfs_mount              fig_sdbfs_mount
#define kiln_sfx                      fig_sfx
#define kiln_sfx_load                 fig_sfx_load
#define kiln_sfx_play                 fig_sfx_play
#define kiln_sfx_play_ex              fig_sfx_play_ex
#define kiln_sfx_playing              fig_sfx_playing
#define kiln_sfx_set_vol_pan          fig_sfx_set_vol_pan
#define kiln_sfx_stop                 fig_sfx_stop
#define kiln_skel                     fig_skel
#define kiln_skel_create              fig_skel_create
#define kiln_skel_destroy             fig_skel_destroy
#define kiln_skel_draw                fig_skel_draw
#define kiln_skel_is_done             fig_skel_is_done
#define kiln_skel_play                fig_skel_play
#define kiln_skel_update              fig_skel_update
#define kiln_splash_apply             fig_splash_apply
#define kiln_splash_done              fig_splash_done
#define kiln_splash_draw2d            fig_splash_draw2d
#define kiln_splash_draw3d            fig_splash_draw3d
#define kiln_splash_init              fig_splash_init
#define kiln_splash_update            fig_splash_update
#define kiln_stream                   fig_stream
#define kiln_surface                  fig_surface
#define kiln_surface_register         fig_surface_register
#define kiln_target                   fig_target
#define kiln_target_acquire           fig_target_acquire
#define kiln_target_draw_reticle      fig_target_draw_reticle
#define kiln_target_switch            fig_target_switch
#define kiln_texanim_draw             fig_texanim_draw
#define kiln_texanim_update           fig_texanim_update
#define kiln_transform_init           fig_transform_init
#define kiln_transform_pop            fig_transform_pop
#define kiln_transform_push           fig_transform_push
#define kiln_video_close              fig_video_close
#define kiln_video_draw               fig_video_draw
#define kiln_video_open               fig_video_open
#define kiln_video_update             fig_video_update
#define kiln_weapon_fire              fig_weapon_fire
#define kiln_weapon_init              fig_weapon_init
#define kiln_weapon_update            fig_weapon_update
#define kiln_weapons_active_def       fig_weapons_active_def
#define kiln_weapons_fire             fig_weapons_fire
#define kiln_weapons_init             fig_weapons_init
#define kiln_weapons_next             fig_weapons_next
#define kiln_weapons_prev             fig_weapons_prev
#define kiln_weapons_update           fig_weapons_update
#define kiln_widget_style_default     fig_widget_style_default
#define kiln_widget_tick              fig_widget_tick

#endif /* FIG_COMPAT_H */
