// SPDX-License-Identifier: MIT
//
// A first-person level with Doom, Half-Life and Ocarina of Time mechanics,
// streamed room by room from three Quake .map files.
//
//   hall      two grunts, NPCs with advice, pickups and barrels; a doorway north
//   corridor  an ambush trigger, a switch, and a gate that only the switch opens
//   arena     grunts and a heavy behind cover, a chest holding the red key, and a
//             red door guarding the exit alcove — reach it to finish the level
//
//   Doom:        weapon switching (pistol/shotgun/rocket/plasma), projectiles,
//                armor, keycard doors, barrels
//   Half-Life:   trigger volumes (the ambush, the exit), NPC dialogue,
//                health + armor
//   OoT:         Z-targeting that turns the view onto the target, a context
//                A-button (talk / open / unlock / use), chests, switch -> door
//
// Engine modules: fig_fpscam fig_weapons fig_clip fig_map fig_room
// fig_actor fig_event fig_surface fig_sound fig_audio fig_gui fig_target
// fig_inventory fig_projectile fig_trigger fig_context fig_dialogue
//
// Left alone for two seconds it plays itself: walks into the hall, turns onto the
// grunts and shoots them, and heads for the corridor doorway. Any input takes
// the pad back.
//
// Jump ROMs (nix/demos/fps.nix):
//   .#fps-switch   in the corridor facing the switch, pressed: the gate rises
//   .#fps-combat   two grunts ahead, the pistol firing into them, held there
//   .#fps-door     at the arena's red door with the red key: A, and it rises

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_clip.h>
#include <kiln/kiln_dict.h>
#include <kiln/kiln_map.h>
#include <kiln/kiln_actor.h>
#include <kiln/kiln_event.h>
#include <kiln/kiln_surface.h>
#include <kiln/kiln_sound.h>
#include <kiln/kiln_audio.h>
#include <kiln/kiln_room.h>
#include <kiln/kiln_fpscam.h>
#include <kiln/kiln_target.h>
#include <kiln/kiln_inventory.h>
#include <kiln/kiln_projectile.h>
#include <kiln/kiln_trigger.h>
#include <kiln/kiln_context.h>
#include <kiln/kiln_dialogue.h>
#include <kiln/kiln_weapons.h>
#include <kiln/kiln_prim.h>

#include <malloc.h>
#include <stdio.h>
#include <string.h>

enum { JUMP_NONE, JUMP_SWITCH, JUMP_COMBAT, JUMP_DOOR };
#ifndef KILN_JUMP
#define KILN_JUMP JUMP_NONE
#endif

#define SCREEN_W 320
#define SCREEN_H 240
#define ACTOR_POOL_CAP 48
#define ROOM_COUNT 3
#define DOOR_RISE 64.0f
#define MAX_WORLD 96

enum { ITEM_KEY_RED = 1 };
enum {
    EV_ENEMY_ATTACK = 0x20,
    EV_DOOR_OPEN    = 0x30,
    EV_CHEST_OPEN   = 0x40,
    EV_AMBUSH       = 257,   /* fps_room1.map's info_trigger event_id */
    EV_EXIT         = 258,   /* fps_room2.map's info_trigger event_id */
};

// ── Profiles ──────────────────────────────────────────────────────────
// PROFILE_START and PROFILE_TRIGGER are map classnames that are never
// spawned as actors: the start is read once for the spawn point, and each
// trigger is installed into fig_trigger at boot.
enum {
    PROFILE_GRUNT, PROFILE_HEAVY, PROFILE_HEALTH_PICKUP, PROFILE_AMMO_PICKUP,
    PROFILE_ARMOR_PICKUP, PROFILE_KEY_PICKUP, PROFILE_NPC,
    PROFILE_CHEST, PROFILE_KEY_DOOR, PROFILE_SWITCH, PROFILE_BARREL,
    PROFILE_PLAYER,
    PROFILE_COUNT,
    PROFILE_START = 0x100, PROFILE_TRIGGER,
};

// ── Models ─────────────────────────────────────────────────────────────
// Every actor is a handful of fig_prim boxes at real size, positioned
// relative to the actor's origin — the flat-shaded cubes this used to draw
// gave a grunt, a door and a health pack the same silhouette.
#define MODEL_PARTS 8
typedef struct { FigPrim part[MODEL_PARTS]; int n; } Model;

static void model_box(Model *m, float ox, float oy, float oz, float hx, float hy, float hz,
                      uint32_t top, uint32_t side)
{
    if (m->n >= MODEL_PARTS) return;
    fig_prim_box(&m->part[m->n++], (fm_vec3_t){{ ox, oy, oz }}, (fm_vec3_t){{ hx, hy, hz }},
                  top, side, fig_prim_shade(side, 0.4f));
}

static void model_draw(const Model *m)
{
    for (int i = 0; i < m->n; i++) fig_prim_draw(&m->part[i]);
}

static Model M_GRUNT, M_HEAVY, M_HEALTH, M_AMMO, M_ARMOR, M_KEY, M_NPC;
static Model M_CHEST_SHUT, M_CHEST_OPEN, M_DOOR, M_GATE, M_SWITCH_OFF, M_SWITCH_ON, M_BARREL;
static Model M_GUN, M_FLASH, M_STRIPE[4];
#define GUN_K     8.0f
#define GUN_SCALE (0.25f / GUN_K)

// ── Globals ────────────────────────────────────────────────────────────
static FigFpsCam g_fpscam;
static FigWeaponSet g_wset;
static FigInventory g_inv;
static FigDialogue g_dialogue;
static FigScene g_scene;
static int g_player_health = 100;
static int g_player_armor = 0;
static int g_score = 0;
static int g_won = 0, g_gate_open = 0, g_exit_open = 0;
static float g_level_time = 0;
static fm_vec3_t g_spawn_pos = {{ 0, 40, -170 }};
static float g_spawn_yaw = 0;

static FigActorHandle g_z_target = FIG_ACTOR_HANDLE_NONE;
static FigContextAction g_ctx_action = FIG_CTX_NONE;
static FigActorHandle g_ctx_actor = FIG_ACTOR_HANDLE_NONE;
static const char *g_toast = NULL;
static float g_toast_t = 0;

static float g_muzzle_flash = 0, g_hitmarker = 0, g_damage_flash = 0;
static float g_shake_t = 0, g_shake_mag = 0;

static void toast(const char *msg) { g_toast = msg; g_toast_t = 2.0f; }

// ── Enemy ──────────────────────────────────────────────────────────────
enum { ENEMY_IDLE, ENEMY_CHASE, ENEMY_ATTACK_S, ENEMY_FLEE };
typedef struct { float speed, attack_cd, scan_cd, strafe_t; uint8_t state; int8_t strafe_sign; int flee_thresh, damage; } EnemyState;

static const fm_vec3_t ENEMY_MINS = {{ -10, -10, -10 }};
static const fm_vec3_t ENEMY_MAXS = {{  10,  10,  10 }};

static void enemy_init(FigActor *self, const FigDict *args)
{
    (void)args;
    EnemyState *s = (void *)self->state;
    if (self->profile_id == PROFILE_HEAVY) {
        s->speed = 28; s->damage = 15; s->flee_thresh = 0; self->health = 200;
    } else {
        s->speed = 48; s->damage = 8; s->flee_thresh = 30; self->health = 100;
    }
    s->attack_cd = 0; s->scan_cd = 0; s->state = ENEMY_IDLE;
    s->strafe_t = 0; s->strafe_sign = 1;
}

static int has_los(fm_vec3_t from, fm_vec3_t to)
{
    return fig_clip_ray(from, to).fraction >= 1.0f;
}

static void enemy_update(FigActor *self, float dt)
{
    EnemyState *s = (void *)self->state;
    fm_vec3_t tp = {{ g_fpscam.pos.v[0]-self->xform.pos.v[0], 0, g_fpscam.pos.v[2]-self->xform.pos.v[2] }};
    float d = fm_vec3_len(&tp);
    s->scan_cd -= dt;
    if (s->scan_cd <= 0) {
        s->scan_cd = 0.4f;
        fm_vec3_t eye = {{ self->xform.pos.v[0], self->xform.pos.v[1]+8, self->xform.pos.v[2] }};
        if (d < 260 && has_los(eye, g_fpscam.pos)) s->state = (d < 34) ? ENEMY_ATTACK_S : ENEMY_CHASE;
        else s->state = ENEMY_IDLE;
        if (self->health < s->flee_thresh && s->flee_thresh > 0) s->state = ENEMY_FLEE;
    }
    s->attack_cd -= dt;
    const float sign = s->state == ENEMY_FLEE ? -1.0f : 1.0f;
    if ((s->state == ENEMY_CHASE || s->state == ENEMY_FLEE) && d > 1) {
        fm_vec3_t dir = {{ sign * tp.v[0]/d, 0, sign * tp.v[2]/d }};
        /* Going round: a straight-line chase slides along a pillar until the
         * pull points into its face and then stops dead — both hall grunts sat
         * pinned against the pillars for the whole level. When a step makes
         * under half its distance, sidestep for a moment, flipping sides if
         * that is blocked too. */
        if (s->strafe_t > 0) {
            s->strafe_t -= dt;
            dir = (fm_vec3_t){{ -dir.v[2] * s->strafe_sign, 0, dir.v[0] * s->strafe_sign }};
        }
        const float step = s->speed * dt;
        const fm_vec3_t before = self->xform.pos;
        self->xform.pos = fig_clip_slide(before, (fm_vec3_t){{ dir.v[0]*step, 0, dir.v[2]*step }},
                                          ENEMY_MINS, ENEMY_MAXS, 4);
        const float mx = self->xform.pos.v[0] - before.v[0], mz = self->xform.pos.v[2] - before.v[2];
        if (mx*mx + mz*mz < 0.25f * step * step) {
            s->strafe_sign = (int8_t)-s->strafe_sign;
            s->strafe_t = 0.6f;
        }
    } else if (s->state == ENEMY_ATTACK_S && s->attack_cd <= 0) {
        s->attack_cd = 0.8f;
        int32_t a[1] = { s->damage };
        fig_event_post(fig_actor_handle_of(self), EV_ENEMY_ATTACK, 200, a, 1, 2);
    }
    self->xform.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
    self->xform.rot_angle = fm_atan2f(sign * tp.v[0], sign * tp.v[2]);
}

static void enemy_event(FigActor *self, uint16_t eid, const int32_t *a, uint8_t c)
{
    if (eid == EV_ENEMY_ATTACK && c >= 1 && !g_won) {
        fm_vec3_t tp = {{ g_fpscam.pos.v[0]-self->xform.pos.v[0], 0, g_fpscam.pos.v[2]-self->xform.pos.v[2] }};
        if (fm_vec3_len(&tp) < 40) {
            int dmg = a[0];
            if (g_player_armor > 0) { g_player_armor -= dmg/2; g_player_health -= dmg - dmg/2; if (g_player_armor<0) g_player_armor=0; }
            else g_player_health -= dmg;
            g_damage_flash = 0.3f; g_shake_t = 0.25f; g_shake_mag = 3;
        }
    }
}

static void grunt_draw(FigActor *s){(void)s;model_draw(&M_GRUNT);}
static void heavy_draw(FigActor *s){(void)s;model_draw(&M_HEAVY);}

// ── Pickups ─────────────────────────────────────────────────────────────
typedef struct { float bob_t; fm_vec3_t home; } PickupState;
static void pickup_init(FigActor *s, const FigDict *a){PickupState*p=(void*)s->state;p->bob_t=0;p->home=s->xform.pos;s->xform.rot_axis=(fm_vec3_t){{0,1,0}};(void)a;}
static int pickup_touch(FigActor *s, float dt){
    PickupState*p=(void*)s->state;p->bob_t+=dt;
    s->xform.pos.v[1]=p->home.v[1]+fm_sinf(p->bob_t*3)*2;s->xform.rot_angle=p->bob_t*1.5f;
    fm_vec3_t tp={{g_fpscam.pos.v[0]-s->xform.pos.v[0],0,g_fpscam.pos.v[2]-s->xform.pos.v[2]}};
    return fm_vec3_len(&tp)<18;
}
static void take(FigActor *s){fig_sound_play("pickup",s->xform.pos,1);fig_actor_despawn(fig_actor_handle_of(s));}
static void health_update(FigActor *s, float dt){ if(pickup_touch(s,dt)&&g_player_health<100){g_player_health+=25;if(g_player_health>100)g_player_health=100;toast("+25 health");take(s);} }
static void ammo_update(FigActor *s, float dt){ if(pickup_touch(s,dt)){FigWeapon*w=fig_weapons_active_state(&g_wset);w->ammo+=24;toast("+24 ammo");take(s);} }
/* Armor caps at 100 — it used to reset to 0 when it passed 100. */
static void armor_update(FigActor *s, float dt){ if(pickup_touch(s,dt)&&g_player_armor<100){g_player_armor+=25;if(g_player_armor>100)g_player_armor=100;toast("+25 armor");take(s);} }
static void key_update(FigActor *s, float dt){ if(pickup_touch(s,dt)){fig_inventory_add(&g_inv,ITEM_KEY_RED,1);toast("red key");take(s);} }
static void health_draw(FigActor*s){(void)s;model_draw(&M_HEALTH);}
static void ammo_draw(FigActor*s){(void)s;model_draw(&M_AMMO);}
static void armor_draw(FigActor*s){(void)s;model_draw(&M_ARMOR);}
static void key_draw(FigActor*s){(void)s;model_draw(&M_KEY);}

// ── NPC ────────────────────────────────────────────────────────────────
typedef struct { const char *line; } NpcState;
static void npc_init(FigActor *s, const FigDict *a){NpcState*n=(void*)s->state;n->line=fig_dict_get_str(a,"dialogue","...");}
static void npc_draw(FigActor *s){(void)s;model_draw(&M_NPC);}

// ── Chest ──────────────────────────────────────────────────────────────
typedef struct { uint8_t open; int contents; } ChestState;
static void chest_init(FigActor *s, const FigDict *a){ChestState*c=(void*)s->state;c->open=0;c->contents=fig_dict_get_int(a,"contents",0);}
static void chest_draw(FigActor *s){ChestState*c=(void*)s->state;model_draw(c->open?&M_CHEST_OPEN:&M_CHEST_SHUT);}
static void chest_event(FigActor *s, uint16_t eid, const int32_t*a, uint8_t c){
    (void)a;(void)c;
    ChestState*cs=(void*)s->state;
    if(eid!=EV_CHEST_OPEN||cs->open)return;
    cs->open=1;fig_sound_play("chest_open",s->xform.pos,1);
    if(cs->contents==ITEM_KEY_RED){
        fm_vec3_t p=s->xform.pos;p.v[1]+=18;
        fig_actor_spawn(PROFILE_KEY_PICKUP,p,0,NULL);
        toast("the chest held a red key");
    }
}

// ── Doors: a portcullis per info_key_door ──────────────────────────────
// key_id 0 is the corridor gate, opened only by its switch; key_id > 0 needs
// that key. Each door carries a level-wide `door_index` stamped at boot in map
// order, which is what an info_switch's `target_door` names.
typedef struct { float cur; uint8_t open; int key_id, door_index; float base_y; } DoorState;
static void door_init(FigActor *s, const FigDict *a){
    DoorState*d=(void*)s->state;d->cur=0;d->open=0;
    d->key_id=fig_dict_get_int(a,"key_id",0);d->door_index=fig_dict_get_int(a,"door_index",-1);
    d->base_y=s->xform.pos.v[1];
    s->health=d->key_id;                       /* fig_context: >0 UNLOCK, 0 OPEN */
}
static void door_update(FigActor *s, float dt){
    DoorState*d=(void*)s->state;float t=3*dt;if(t>1)t=1;
    d->cur+=((d->open?1.0f:0.0f)-d->cur)*t;
    s->xform.pos.v[1]=d->base_y+d->cur*DOOR_RISE;
}
static void door_draw(FigActor *s){DoorState*d=(void*)s->state;model_draw(d->key_id==0?&M_GATE:&M_DOOR);}
static void door_event(FigActor *s, uint16_t eid, const int32_t*a, uint8_t c){
    (void)a;(void)c;
    if(eid!=EV_DOOR_OPEN)return;
    DoorState*d=(void*)s->state;if(d->open)return;
    d->open=1;s->health=-1;                    /* no context action once open */
    if(d->key_id==0)g_gate_open=1; else g_exit_open=1;
    fig_sound_play("door_open",s->xform.pos,1);
}

static FigActor *find_door(int door_index)
{
    for (FigActor *a = fig_actor_first(FIG_ACTOR_CAT_DOOR); a; a = fig_actor_next(a))
        if (((DoorState *)a->state)->door_index == door_index) return a;
    return NULL;
}

// ── Switch ─────────────────────────────────────────────────────────────
typedef struct { int target_door; uint8_t activated; } SwitchState;
static void switch_init(FigActor *s, const FigDict *a){SwitchState*sw=(void*)s->state;sw->target_door=fig_dict_get_int(a,"target_door",-1);sw->activated=0;}
static void switch_draw(FigActor *s){SwitchState*sw=(void*)s->state;model_draw(sw->activated?&M_SWITCH_ON:&M_SWITCH_OFF);}

// ── Barrel ─────────────────────────────────────────────────────────────
typedef struct { float hp; } BarrelState;
static void barrel_init(FigActor *s, const FigDict *a){BarrelState*b=(void*)s->state;b->hp=30;s->health=30;(void)a;}
static void barrel_draw(FigActor *s){(void)s;model_draw(&M_BARREL);}
static void barrel_event(FigActor *s, uint16_t eid, const int32_t *a, uint8_t c){
    if(eid!=EV_ENEMY_ATTACK||c<1)return;
    BarrelState*b=(void*)s->state;b->hp-=(float)a[0];s->health=(int)b->hp;
    if(b->hp>0)return;
    fig_sound_play("explosion",s->xform.pos,1);
    g_shake_t=0.35f;g_shake_mag=5;
    for(FigActor*e=fig_actor_first(FIG_ACTOR_CAT_ENEMY);e;){
        FigActor*next=fig_actor_next(e);
        fm_vec3_t d={{e->xform.pos.v[0]-s->xform.pos.v[0],0,e->xform.pos.v[2]-s->xform.pos.v[2]}};
        if(fm_vec3_len(&d)<48){e->health-=60;if(e->health<=0){fig_actor_despawn(fig_actor_handle_of(e));g_score++;}}
        e=next;
    }
    fig_actor_despawn(fig_actor_handle_of(s));
}

// ── Player (receives trigger events) ───────────────────────────────────
static void player_event(FigActor *self, uint16_t eid, const int32_t *a, uint8_t c)
{
    (void)self; (void)a; (void)c;
    if (eid == EV_AMBUSH) {
        /* Two grunts, ahead of and behind the player along the corridor. */
        for (int i = -1; i <= 1; i += 2) {
            fm_vec3_t p = {{ g_fpscam.pos.v[0], 12, g_fpscam.pos.v[2] + i * 70.0f }};
            fig_actor_spawn_in_room(PROFILE_GRUNT, p, 0, 1, NULL);
        }
        toast("AMBUSH!");
    } else if (eid == EV_EXIT && !g_won) {
        g_won = 1;
        fig_sound_play("pickup", g_fpscam.pos, 1);
    }
}

static const FigActorProfile PROFILES[PROFILE_COUNT] = {
    [PROFILE_GRUNT]  = { .name="grunt", .category=FIG_ACTOR_CAT_ENEMY, .state_size=sizeof(EnemyState), .init=enemy_init, .update=enemy_update, .draw=grunt_draw, .event=enemy_event },
    [PROFILE_HEAVY]  = { .name="heavy", .category=FIG_ACTOR_CAT_ENEMY, .state_size=sizeof(EnemyState), .init=enemy_init, .update=enemy_update, .draw=heavy_draw, .event=enemy_event },
    [PROFILE_HEALTH_PICKUP] = { .name="health", .category=FIG_ACTOR_CAT_ITEM, .state_size=sizeof(PickupState), .init=pickup_init, .update=health_update, .draw=health_draw },
    [PROFILE_AMMO_PICKUP]   = { .name="ammo", .category=FIG_ACTOR_CAT_ITEM, .state_size=sizeof(PickupState), .init=pickup_init, .update=ammo_update, .draw=ammo_draw },
    [PROFILE_ARMOR_PICKUP]  = { .name="armor", .category=FIG_ACTOR_CAT_ITEM, .state_size=sizeof(PickupState), .init=pickup_init, .update=armor_update, .draw=armor_draw },
    [PROFILE_KEY_PICKUP]    = { .name="key", .category=FIG_ACTOR_CAT_ITEM, .state_size=sizeof(PickupState), .init=pickup_init, .update=key_update, .draw=key_draw },
    [PROFILE_NPC]           = { .name="npc", .category=FIG_ACTOR_CAT_NPC, .state_size=sizeof(NpcState), .init=npc_init, .draw=npc_draw },
    [PROFILE_CHEST]         = { .name="chest", .category=FIG_ACTOR_CAT_CHEST, .state_size=sizeof(ChestState), .init=chest_init, .draw=chest_draw, .event=chest_event },
    [PROFILE_KEY_DOOR]      = { .name="keydoor", .category=FIG_ACTOR_CAT_DOOR, .state_size=sizeof(DoorState), .init=door_init, .update=door_update, .draw=door_draw, .event=door_event },
    [PROFILE_SWITCH]        = { .name="switch", .category=FIG_ACTOR_CAT_PROP, .state_size=sizeof(SwitchState), .init=switch_init, .draw=switch_draw },
    [PROFILE_BARREL]        = { .name="barrel", .category=FIG_ACTOR_CAT_PROP, .state_size=sizeof(BarrelState), .init=barrel_init, .draw=barrel_draw, .event=barrel_event },
    [PROFILE_PLAYER]        = { .name="player", .category=FIG_ACTOR_CAT_PLAYER, .event=player_event },
};

static FigActor g_pool[ACTOR_POOL_CAP];

// ── Room streaming ─────────────────────────────────────────────────────
// Three rooms end to end along +Z. The clip world is composed here each frame
// from the loaded rooms' brushes plus every CLOSED door, so a door blocks while
// shut — fig_room's own installer only knows about static room brushes.
static FigRoom g_rooms[ROOM_COUNT];
static FigRoomSystem g_room_sys;
static FigMap g_room_maps[ROOM_COUNT];
static int g_room_loaded[ROOM_COUNT];
static FigBrush g_world[MAX_WORLD];

static void on_room_load(FigRoom *room, void *user)
{
    (void)user;
    if (g_room_loaded[room->id]) return;
    char path[64];
    snprintf(path, sizeof(path), "rom:/maps/fps_room%d.map", room->id);
    if (fig_map_load(&g_room_maps[room->id], path) < 0) return;
    room->brushes = g_room_maps[room->id].brushes;
    room->brush_count = g_room_maps[room->id].brush_count;
    room->user_mesh = &g_room_maps[room->id];
    /* Each room its own material: warm stone hall, steel corridor, rust arena. */
    static const FigMapTint TINT[ROOM_COUNT] = {
        { .floor = 0x7A6A58FF, .floor_edge = 0x5A4C40FF, .floor_y = 0.5f, .floor_radius = 400,
          .top = 0xC8B898FF, .wall_low = 0x4A4038FF, .wall_high = 0xB0A088FF, .z_face_shade = 0.85f, .underside = 0x282420FF },
        { .floor = 0x5A6470FF, .floor_edge = 0x3C444EFF, .floor_y = 0.5f, .floor_radius = 400,
          .top = 0xA8B4C4FF, .wall_low = 0x343C48FF, .wall_high = 0x8C98A8FF, .z_face_shade = 0.85f, .underside = 0x202428FF },
        { .floor = 0x7A5040FF, .floor_edge = 0x4C3028FF, .floor_y = 0.5f, .floor_radius = 700,
          .top = 0xD0A070FF, .wall_low = 0x4A2C24FF, .wall_high = 0xB07858FF, .z_face_shade = 0.85f, .underside = 0x281814FF },
    };
    fig_map_tint(&g_room_maps[room->id], &TINT[room->id]);
    g_room_loaded[room->id] = 1;
}
static void on_room_unload(FigRoom *room, void *user)
{
    (void)user;
    if (!g_room_loaded[room->id]) return;
    fig_map_free(&g_room_maps[room->id]);
    room->brushes = NULL; room->brush_count = 0; room->user_mesh = NULL;
    g_room_loaded[room->id] = 0;
}
static void on_room_spawn(FigRoom *room, const FigRoomSpawn *spawn, void *user)
{
    (void)user;
    if (spawn->profile_id >= PROFILE_COUNT) return;   /* start / trigger */
    fig_actor_spawn_in_room(spawn->profile_id, spawn->pos, spawn->yaw, room->id, &spawn->dict);
}
static void on_room_draw(FigRoom *room, void *user)
{
    (void)user;
    if (room->user_mesh) fig_map_draw((const FigMap *)room->user_mesh);
}

static void compose_world(void)
{
    uint16_t n = 0;
    for (FigRoom *r = fig_room_first_loaded(&g_room_sys); r; r = fig_room_next_loaded(&g_room_sys, r))
        for (uint16_t i = 0; i < r->brush_count && n < MAX_WORLD; i++) g_world[n++] = r->brushes[i];
    for (FigActor *a = fig_actor_first(FIG_ACTOR_CAT_DOOR); a && n < MAX_WORLD; a = fig_actor_next(a)) {
        const DoorState *d = (const DoorState *)a->state;
        if (d->open) continue;
        const fm_vec3_t p = {{ a->xform.pos.v[0], d->base_y, a->xform.pos.v[2] }};
        g_world[n++] = (FigBrush){ .mins = {{ p.v[0] - 24, p.v[1] - 36, p.v[2] - 4 }},
                                    .maxs = {{ p.v[0] + 24, p.v[1] + 36, p.v[2] + 4 }} };
    }
    fig_clip_set_world(g_world, n);
}

// ── Weapons ────────────────────────────────────────────────────────────
static const FigWeaponDef WEAPON_DEFS[] = {
    { "Pistol",  FIG_WTYPE_HITSCAN,    0, 12, 0.15f, 1.5f, 34, 0,   "gunshot",      0xFFD94CFF },
    { "Shotgun", FIG_WTYPE_HITSCAN,    0, 6,  0.6f,  2.0f, 20, 0,   "shotgun_fire", 0xFF8844FF },
    { "Rocket",  FIG_WTYPE_PROJECTILE, FIG_PROJ_ROCKET_W, 1, 0.8f, 2.5f, 80, 40, "rocket_fire",  0xFF4444FF },
    { "Plasma",  FIG_WTYPE_PROJECTILE, FIG_PROJ_PLASMA_W, 20, 0.1f, 1.0f, 25, 0,  "plasma_fire",  0x44FFFFFF },
};

static void damage_enemy(FigActor *a, int damage)
{
    a->health -= damage;
    fig_sound_play("enemy_hit", a->xform.pos, 1);
    g_hitmarker = 0.15f;
    if (a->health <= 0) { fig_actor_despawn(fig_actor_handle_of(a)); g_score++; }
}

static void proj_hit_cb(uint16_t profile_id, fm_vec3_t pos, int damage, float radius)
{
    (void)profile_id;
    const float r = radius > 20 ? radius : 20;
    fig_sound_play("impact", pos, 1);
    for (FigActor *a = fig_actor_first(FIG_ACTOR_CAT_ENEMY); a; ) {
        FigActor *next = fig_actor_next(a);
        fm_vec3_t d = {{ a->xform.pos.v[0]-pos.v[0], 0, a->xform.pos.v[2]-pos.v[2] }};
        if (fm_vec3_len(&d) < r) damage_enemy(a, damage);
        a = next;
    }
    for (FigActor *b = fig_actor_first(FIG_ACTOR_CAT_PROP); b; b = fig_actor_next(b)) {
        if (b->profile_id != PROFILE_BARREL) continue;
        fm_vec3_t d = {{ b->xform.pos.v[0]-pos.v[0], 0, b->xform.pos.v[2]-pos.v[2] }};
        if (fm_vec3_len(&d) < r + 5) {
            int32_t args[1] = { 50 };
            fig_event_post(fig_actor_handle_of(b), EV_ENEMY_ATTACK, 0, args, 1, 3);
        }
    }
}

static int ray_hits_actor(fm_vec3_t o, fm_vec3_t d, FigActor *a, float r, float max_t)
{
    fm_vec3_t tc={{a->xform.pos.v[0]-o.v[0],a->xform.pos.v[1]-o.v[1],a->xform.pos.v[2]-o.v[2]}};
    float t=tc.v[0]*d.v[0]+tc.v[1]*d.v[1]+tc.v[2]*d.v[2];
    if(t<0||t>max_t)return 0;
    float dx=tc.v[0]-d.v[0]*t,dy=tc.v[1]-d.v[1]*t,dz=tc.v[2]-d.v[2]*t;
    return (dx*dx+dy*dy+dz*dz)<r*r;
}

static void do_hitscan(int damage)
{
    fm_vec3_t o = g_fpscam.pos;
    fm_vec3_t d = fig_fpscam_forward(&g_fpscam);
    fm_vec3_t end = {{ o.v[0]+d.v[0]*600, o.v[1]+d.v[1]*600, o.v[2]+d.v[2]*600 }};
    FigTrace tr = fig_clip_ray(o, end);
    const float wall_t = tr.fraction * 600;      /* nothing behind a wall is hit */
    for (FigActor *a = fig_actor_first(FIG_ACTOR_CAT_ENEMY); a; a = fig_actor_next(a)) {
        float r = (a->profile_id == PROFILE_HEAVY) ? 16 : 12;
        if (ray_hits_actor(o, d, a, r, wall_t)) { damage_enemy(a, damage); return; }
    }
    for (FigActor *b = fig_actor_first(FIG_ACTOR_CAT_PROP); b; b = fig_actor_next(b)) {
        if (b->profile_id == PROFILE_BARREL && ray_hits_actor(o, d, b, 12, wall_t)) {
            int32_t args[1] = { damage };
            fig_event_post(fig_actor_handle_of(b), EV_ENEMY_ATTACK, 0, args, 1, 3);
            return;
        }
    }
    if (tr.fraction < 1) fig_sound_play("impact", tr.endpos, 1);
}

static void fire_weapon(void)
{
    if (!fig_weapons_can_fire(&g_wset)) return;
    if (!fig_weapons_fire(&g_wset)) return;
    const FigWeaponDef *def = fig_weapons_active_def(&g_wset);
    fig_sound_play(def->sfx_name, g_fpscam.pos, 1);
    g_muzzle_flash = 0.06f;
    if (def->type == FIG_WTYPE_HITSCAN) {
        do_hitscan(def->damage);
    } else {
        fm_vec3_t fwd = fig_fpscam_forward(&g_fpscam);
        fm_vec3_t vel = {{ fwd.v[0]*400, fwd.v[1]*400, fwd.v[2]*400 }};
        fig_projectile_spawn(def->proj_type, g_fpscam.pos, vel, 3.0f, def->damage, def->splash_radius);
    }
}

// ── Tape ────────────────────────────────────────────────────────────────
static const FigInputKey SWITCH_KEYS[] = {
    { .frame = 0 },
    { .frame = 40, .buttons = FIG_BTN_A },
    { .frame = 46 },
};
static const FigInputTape SWITCH_TAPE = { SWITCH_KEYS, 3, FIG_INPUT_NO_LOOP };
/* COMBAT: the pistol held down from the moment the grunts are placed. */
static const FigInputKey COMBAT_KEYS[] = {
    { .frame = 0 },
    { .frame = 20, .buttons = FIG_BTN_A },
    { .frame = 21, .buttons = FIG_BTN_A },
};
static const FigInputTape COMBAT_TAPE = { COMBAT_KEYS, 3, FIG_INPUT_NO_LOOP };

/* DOOR: one press of A at the red door, with the key already carried. */
static const FigInputKey DOOR_KEYS[] = {
    { .frame = 0 },
    { .frame = 40, .buttons = FIG_BTN_A },   /* one frame: held, A would fire */
    { .frame = 41 },
    { .frame = 70, .sy = -60 },               /* step back to see it risen */
    { .frame = 88 },
};
static const FigInputTape DOOR_TAPE = { DOOR_KEYS, 5, FIG_INPUT_NO_LOOP };

/* Attract, from the frame it takes over (boot + 2 s idle). Sticks are raw
 * counts: +sx strafes right, +sy walks forward, +cx turns right, -cy looks
 * down. */
static const FigInputKey ATTRACT_KEYS[] = {
    { .frame = 0,   .cx = 85, .cy = -85 },                            /* onto the right grunt */
    { .frame = 10,  .cx = 85 },
    { .frame = 13,  .buttons = FIG_BTN_Z },                          /* lock */
    { .frame = 16,  .buttons = FIG_BTN_Z | FIG_BTN_A },             /* and fire */
    { .frame = 52 },
    { .frame = 55,  .cx = -85 },                                      /* round to the other */
    { .frame = 80,  .buttons = FIG_BTN_Z, .cy = 85 },
    { .frame = 86,  .buttons = FIG_BTN_Z | FIG_BTN_A, .sy = -40, .sx = 60 },   /* kite, firing */
    { .frame = 180, .buttons = FIG_BTN_Z | FIG_BTN_CD, .sy = -40, .sx = -60 }, /* reload */
    { .frame = 182, .buttons = FIG_BTN_Z, .sy = -40, .sx = -60 },
    { .frame = 280, .buttons = FIG_BTN_Z | FIG_BTN_A, .sy = -40, .sx = 60 },
    { .frame = 380, .buttons = FIG_BTN_Z | FIG_BTN_CD, .sy = -40, .sx = -60 },
    { .frame = 382, .buttons = FIG_BTN_Z, .sy = -40, .sx = -60 },
    { .frame = 420, .buttons = FIG_BTN_Z | FIG_BTN_A, .sy = -40, .sx = 60 },
    { .frame = 520, .buttons = FIG_BTN_Z | FIG_BTN_A, .cy = -85 },  /* aim low: finish it */
    { .frame = 526, .buttons = FIG_BTN_Z | FIG_BTN_A },
    { .frame = 580, .cy = 85 },                                       /* level the view */
    { .frame = 590, .cx = -85 },                                      /* face the doorway */
    { .frame = 626 },
    { .frame = 630, .sy = 85 },                                       /* and go */
    { .frame = 684, .sy = 85, .cx = 85 },
    { .frame = 694, .sy = 85 },
    { .frame = 715 },            /* stop short of the corridor's ambush trigger */
};
static const FigInputTape ATTRACT_TAPE = { ATTRACT_KEYS, sizeof ATTRACT_KEYS / sizeof ATTRACT_KEYS[0], FIG_INPUT_NO_LOOP };


static void build_models(void)
{
    const uint32_t GLOW = fig_prim_rgba(0xFF, 0xF0, 0x60);
    /* Grunt: origin at its centre, 10 above the floor. */
    model_box(&M_GRUNT, 0, -1, 0, 8, 9, 6, 0xD83030FF, 0xA02020FF);
    model_box(&M_GRUNT, 0, 12, 0, 6, 4, 6, 0x802020FF, 0x601818FF);
    model_box(&M_GRUNT, -3, 12, 6, 2, 1, 1, GLOW, GLOW);
    model_box(&M_GRUNT,  3, 12, 6, 2, 1, 1, GLOW, GLOW);
    /* Heavy: bigger, darker, armoured shoulders. */
    model_box(&M_HEAVY, 0, -2, 0, 12, 12, 9, 0x804040FF, 0x582828FF);
    model_box(&M_HEAVY, 0, 14, 0, 7, 5, 7, 0x484850FF, 0x303038FF);
    model_box(&M_HEAVY, -15, 6, 0, 4, 4, 7, 0x9098A8FF, 0x606878FF);
    model_box(&M_HEAVY,  15, 6, 0, 4, 4, 7, 0x9098A8FF, 0x606878FF);
    model_box(&M_HEAVY, -3, 15, 7, 2, 1, 1, 0xFF6040FF, 0xFF6040FF);
    model_box(&M_HEAVY,  3, 15, 7, 2, 1, 1, 0xFF6040FF, 0xFF6040FF);
    /* Pickups. */
    model_box(&M_HEALTH, 0, 0, 0, 6, 6, 6, 0xF8F8F8FF, 0xD8D8D8FF);
    model_box(&M_HEALTH, 0, 0, 0, 4, 2, 7, 0xE02020FF, 0xE02020FF);
    model_box(&M_HEALTH, 0, 0, 0, 2, 4, 7, 0xE02020FF, 0xE02020FF);
    model_box(&M_AMMO, 0, 0, 0, 7, 4, 5, 0x6A7040FF, 0x505430FF);
    model_box(&M_AMMO, 0, 1, 0, 8, 1, 6, 0xF0D040FF, 0xD0B030FF);
    model_box(&M_ARMOR, 0, 0, 0, 6, 7, 2, 0x4070E0FF, 0x3050B0FF);
    model_box(&M_ARMOR, 0, 1, 0, 4, 5, 3, 0x90B8FFFF, 0x7090D0FF);
    model_box(&M_KEY, 0, 4, 0, 3, 3, 1, 0xFF5050FF, 0xD03030FF);
    model_box(&M_KEY, 0, -2, 0, 1, 4, 1, 0xFFD060FF, 0xD0A040FF);
    model_box(&M_KEY, 2, -5, 0, 2, 1, 1, 0xFFD060FF, 0xD0A040FF);
    /* NPC: robe, belt, head, hood. */
    model_box(&M_NPC, 0, 0, 0, 7, 12, 6, 0x88B8E8FF, 0x5888C0FF);
    model_box(&M_NPC, 0, -2, 0, 8, 1, 7, 0xD0A050FF, 0xA07830FF);
    model_box(&M_NPC, 0, 16, 0, 5, 5, 5, 0xF0C8A0FF, 0xD8B088FF);
    model_box(&M_NPC, 0, 21, -1, 6, 2, 6, 0x5888C0FF, 0x406898FF);
    /* Chest: body, band, and a lid that stands up when opened. */
    model_box(&M_CHEST_SHUT, 0, -5, 0, 12, 7, 8, 0x8B5A2BFF, 0x6A4020FF);
    model_box(&M_CHEST_SHUT, 0, -4, 0, 13, 1, 9, 0xF0C040FF, 0xC89830FF);
    model_box(&M_CHEST_SHUT, 0, 4, 0, 13, 3, 9, 0xA06A38FF, 0x7A5028FF);
    model_box(&M_CHEST_OPEN, 0, -5, 0, 12, 7, 8, 0x8B5A2BFF, 0x6A4020FF);
    model_box(&M_CHEST_OPEN, 0, -4, 0, 13, 1, 9, 0xF0C040FF, 0xC89830FF);
    model_box(&M_CHEST_OPEN, 0, 11, -9, 13, 9, 2, 0xA06A38FF, 0x7A5028FF);
    model_box(&M_CHEST_OPEN, 0, 2, 0, 10, 1, 6, GLOW, GLOW);
    /* The red door and the corridor's portcullis, at their doorway's size. */
    model_box(&M_DOOR, 0, 0, 0, 24, 36, 3, 0xB83030FF, 0x902020FF);
    model_box(&M_DOOR, 0, 0, 4, 5, 5, 1, 0xF0C040FF, 0xC89830FF);
    model_box(&M_DOOR, -16, 24, 4, 2, 2, 1, 0x505050FF, 0x404040FF);
    model_box(&M_DOOR,  16, 24, 4, 2, 2, 1, 0x505050FF, 0x404040FF);
    for (int i = -2; i <= 2; i++) model_box(&M_GATE, i * 10.0f, 0, 0, 2, 36, 3, 0x8890A0FF, 0x586070FF);
    model_box(&M_GATE, 0, 20, 0, 24, 2, 3, 0x8890A0FF, 0x586070FF);
    model_box(&M_GATE, 0, -20, 0, 24, 2, 3, 0x8890A0FF, 0x586070FF);
    /* Switch: a pedestal with a button that goes teal once pressed. */
    model_box(&M_SWITCH_OFF, 0, -12, 0, 6, 8, 6, 0xB0B4C0FF, 0x787C88FF);
    model_box(&M_SWITCH_OFF, 0, -2, 0, 4, 2, 4, 0xFF4466FF, 0xC02040FF);
    model_box(&M_SWITCH_ON, 0, -12, 0, 6, 8, 6, 0xB0B4C0FF, 0x787C88FF);
    model_box(&M_SWITCH_ON, 0, -3, 0, 4, 1, 4, 0x40FFD8FF, 0x20C0A0FF);
    /* Barrel: orange drum, dark hoops, a cap. */
    model_box(&M_BARREL, 0, 0, 0, 8, 8, 8, 0xFF8C20FF, 0xD06010FF);
    model_box(&M_BARREL, 0, -4, 0, 9, 1, 9, 0x303030FF, 0x282828FF);
    model_box(&M_BARREL, 0, 4, 0, 9, 1, 9, 0x303030FF, 0x282828FF);
    model_box(&M_BARREL, 0, 9, 0, 6, 1, 6, 0x484848FF, 0x383838FF);
    /* The gun, in the camera's frame: -X is screen-right, +Z is forward, in
     * GUN_K-times units so its sub-unit parts survive fig_prim's integer
     * positions. Laid out 17..35 ahead and 9 right of the eye, it projects to
     * a pistol in the bottom-right quarter; draw_viewmodel shrinks the whole
     * thing about the eye, which changes nothing on screen. */
    #define GB(m, x, y, z, hx, hy, hz, top, side) model_box((m), (x)*GUN_K, (y)*GUN_K, (z)*GUN_K, \
                                                   (hx)*GUN_K, (hy)*GUN_K, (hz)*GUN_K, (top), (side))
    GB(&M_GUN, -9.0f, -6.6f, 22.0f, 1.6f, 1.4f, 5.0f, 0xA8B0C0FF, 0x707888FF);   /* slide  */
    GB(&M_GUN, -9.0f, -5.8f, 31.0f, 0.7f, 0.7f, 4.0f, 0x8890A0FF, 0x586070FF);   /* barrel */
    GB(&M_GUN, -9.0f, -10.0f, 18.8f, 1.1f, 2.6f, 1.4f, 0x9A7050FF, 0x6A4A34FF);  /* grip   */
    GB(&M_GUN, -9.0f, -4.9f, 33.5f, 0.3f, 0.5f, 0.3f, 0xE0E4F0FF, 0xA0A4B0FF);   /* sight  */
    GB(&M_FLASH, -9.0f, -5.8f, 35.6f, 1.6f, 1.6f, 0.4f, 0xFFFFE0FF, 0xFFE080FF);
    /* One stripe colour per weapon slot, so a switch reads on the gun itself. */
    static const uint32_t STRIPE[4] = { 0xFFD94CFF, 0xFF8844FF, 0xFF4444FF, 0x44FFFFFF };
    for (int i = 0; i < 4; i++) GB(&M_STRIPE[i], -9.0f, -5.1f, 22.0f, 0.9f, 0.2f, 3.2f, STRIPE[i], STRIPE[i]);
    #undef GB
}

/* The first-person gun, in the view's own frame, so it stays in the same place
 * on screen however the view moves.
 *
 * It was laid out 7..21 units ahead at 1x. The near end of its barrel was
 * closer to the eye than the barrel was wide, so it ran off the bottom-right
 * corner as a slab across a quarter of the screen — the same size on the host
 * and in Ares at yaw 0; the console's dark top face only made it read bigger.
 * It is now laid out 17..35 ahead in build_models and drawn at GUN_SCALE about
 * the eye. Scaling about the eye changes nothing that projects to the screen,
 * but it puts the gun's nearest point 4.4 units away (just past near_z 4) and
 * its muzzle 8.9, inside the player's 10-unit collision box, so no wall can
 * come between the eye and the gun.
 *
 * One matrix built from the view basis, not a yaw FigTransform pushed under a
 * pitch one. fig_transform_push rotates with fm_mat4_from_axis_angle, which
 * builds the transpose of fm_mat4_from_srt's rotation (it turns by -angle), and
 * the host backend composes a nested push child x parent where Tiny3D's ucode
 * composes parent x child. So the gun left its place as soon as the view turned
 * or pitched, and differently on each renderer: gone on the host, a misplaced
 * slab in Ares. */
static void draw_viewmodel(void)
{
    static T3DMat4FP *mtx;
    if (!mtx) {
        mtx = malloc_uncached(sizeof *mtx);
        assertf(mtx, "fps: out of memory for the viewmodel matrix");
    }
    /* The view's own basis, built the way t3d_viewport_look_at builds it:
     * side = forward x up is screen-right. The gun's -X is screen-right, so
     * its X column is -side; Y is the view's up and Z its forward. */
    const fm_vec3_t f = fig_fpscam_forward(&g_fpscam);
    fm_vec3_t side = {{ -f.v[2], 0, f.v[0] }};
    fm_vec3_norm(&side, &side);
    fm_vec3_t up;
    fm_vec3_cross(&up, &side, &f);
    const float kick = g_muzzle_flash > 0 ? -0.25f : 0.0f;
    fm_mat4_t m;
    memset(&m, 0, sizeof m);
    for (int i = 0; i < 3; i++) {
        m.m[0][i] = -side.v[i] * GUN_SCALE;
        m.m[1][i] = up.v[i] * GUN_SCALE;
        m.m[2][i] = f.v[i] * GUN_SCALE;
        m.m[3][i] = g_scene.cam_pos.v[i] + f.v[i] * kick;
    }
    m.m[3][3] = 1.0f;
    t3d_mat4_to_fixed(mtx, &m);
    t3d_matrix_push(mtx);
    model_draw(&M_GUN);
    const FigWeaponDef *wd = fig_weapons_active_def(&g_wset);
    const int wi = wd ? (int)(wd - WEAPON_DEFS) : 0;
    if (wi >= 0 && wi < 4) model_draw(&M_STRIPE[wi]);
    if (g_muzzle_flash > 0) model_draw(&M_FLASH);
    t3d_matrix_pop(1);
}

/* Jump ROMs place their state once the rooms have spawned their actors — the
 * first frame, not before the loop — and COMBAT holds it there: the grunts
 * cannot die, the player cannot, and the magazine never runs dry, so a capture
 * at any boot time finds the same fight. */
static void jump_setup(void)
{
    if (KILN_JUMP == JUMP_COMBAT) {
        static const fm_vec3_t AT[2] = { {{ 0, 12, -95 }}, {{ -26, 12, -104 }} };
        int i = 0;
        for (FigActor *e = fig_actor_first(FIG_ACTOR_CAT_ENEMY); e && i < 2; e = fig_actor_next(e), i++) {
            e->xform.pos = AT[i];
            ((EnemyState *)e->state)->speed = 0;
        }
    } else if (KILN_JUMP == JUMP_DOOR) {
        /* The arena already cleared: the heavy stands where the player does. */
        for (FigActor *e = fig_actor_first(FIG_ACTOR_CAT_ENEMY); e; ) {
            FigActor *next = fig_actor_next(e);
            fig_actor_despawn(fig_actor_handle_of(e));
            e = next;
        }
        fig_inventory_add(&g_inv, ITEM_KEY_RED, 1);
        g_gate_open = 1;
    }
}

static void jump_latch(void)
{
    if (KILN_JUMP != JUMP_COMBAT) return;
    g_player_health = 100;
    for (FigActor *e = fig_actor_first(FIG_ACTOR_CAT_ENEMY); e; e = fig_actor_next(e))
        if (e->health < 100) e->health = 100;
    FigWeapon *w = fig_weapons_active_state(&g_wset);
    if (w) { w->magazine = w->magazine_size; w->ammo = 36; }
}

static void respawn(void)
{
    g_player_health = 100; g_player_armor = 0;
    fig_fpscam_snap(&g_fpscam, g_spawn_pos, g_spawn_yaw, 0);
}

static const char *objective(void)
{
    if (g_won) return "level complete";
    if (!g_gate_open) return "find the switch in the corridor";
    /* The key is spent opening the door: without this the objective went back
     * to "find the red key" the moment the door rose. */
    if (g_exit_open) return "red door open: reach the exit";
    if (!fig_inventory_has(&g_inv, ITEM_KEY_RED)) return "find the red key";
    return "open the red door, reach the exit";
}

int main(void)
{
    fig_engine_init(RESOLUTION_320x240);
    joypad_init();
    dfs_init(DFS_DEFAULT_LOCATION);
    fig_input_init();
    fig_audio_init(FIG_AUDIO_DEFAULT);

    int sfx_imp = fig_sfx_load("rom:/sfx/impact.wav64");
    int sfx_met = fig_sfx_load("rom:/sfx/impact_metal.wav64");
    fig_surface_register(0, &(FigSurfaceDef){.friction=0.9f,.footstep_sfx=sfx_imp});
    fig_surface_register(1, &(FigSurfaceDef){.friction=1.0f,.footstep_sfx=sfx_met});

    FigSoundShader shaders[] = {
        {"gunshot","rom:/sfx/gunshot.wav64",0.8f,0,0},
        {"impact","rom:/sfx/impact.wav64",0.5f,0,0},
        {"enemy_hit","rom:/sfx/enemy_hit.wav64",0.6f,200,0},
        {"pickup","rom:/sfx/pickup.wav64",0.6f,150,0},
        {"door_open","rom:/sfx/door_open.wav64",0.5f,150,0},
        {"door_locked","rom:/sfx/door_locked.wav64",0.4f,150,0},
        {"chest_open","rom:/sfx/chest_open.wav64",0.5f,150,0},
        {"explosion","rom:/sfx/explosion.wav64",0.8f,300,0},
        {"rocket_fire","rom:/sfx/rocket_fire.wav64",0.6f,0,0},
        {"plasma_fire","rom:/sfx/plasma_fire.wav64",0.4f,0,0},
        {"shotgun_fire","rom:/sfx/shotgun_fire.wav64",0.7f,0,0},
        {"npc_talk","rom:/sfx/npc_talk.wav64",0.3f,100,0},
    };
    fig_sound_init(shaders, 12);

    /* One actor system, with the player's event-only profile in the table.
     * This used to spawn a dummy of profile 0xFFFF before the system existed
     * and then re-initialise the pool with an extended table. */
    fig_actor_system_init(PROFILES, PROFILE_COUNT, g_pool, ACTOR_POOL_CAP);
    fig_event_init();
    fig_projectile_init();
    fig_projectile_set_hit_fn(proj_hit_cb);
    fig_trigger_init();
    fig_inventory_init(&g_inv);
    memset(&g_dialogue, 0, sizeof(g_dialogue));

    fig_map_register_classname("info_enemy", PROFILE_GRUNT);
    fig_map_register_classname("info_heavy", PROFILE_HEAVY);
    fig_map_register_classname("info_health", PROFILE_HEALTH_PICKUP);
    fig_map_register_classname("info_ammo", PROFILE_AMMO_PICKUP);
    fig_map_register_classname("info_armor", PROFILE_ARMOR_PICKUP);
    fig_map_register_classname("info_key_red", PROFILE_KEY_PICKUP);
    fig_map_register_classname("info_npc", PROFILE_NPC);
    fig_map_register_classname("info_chest", PROFILE_CHEST);
    fig_map_register_classname("info_key_door", PROFILE_KEY_DOOR);
    fig_map_register_classname("info_switch", PROFILE_SWITCH);
    fig_map_register_classname("info_barrel", PROFILE_BARREL);
    /* The start was registered as a GRUNT, so an enemy spawned on the player. */
    fig_map_register_classname("info_player_start", PROFILE_START);
    fig_map_register_classname("info_trigger", PROFILE_TRIGGER);

    /* Hall, corridor, arena, end to end along +Z; each AABB covers its map. */
    memset(g_rooms, 0, sizeof(g_rooms));
    const struct { float x0, z0, x1, z1; } R[ROOM_COUNT] = {
        { -170, -210, 170,   0 }, { -60, 0, 60, 240 }, { -210, 240, 210, 540 },
    };
    for (int i = 0; i < ROOM_COUNT; i++) {
        g_rooms[i].id = (uint8_t)i;
        g_rooms[i].aabb_min = (fm_vec3_t){{ R[i].x0, 0, R[i].z0 }};
        g_rooms[i].aabb_max = (fm_vec3_t){{ R[i].x1, 100, R[i].z1 }};
    }
    g_rooms[0].neighbours[0] = 1; g_rooms[0].neighbour_count = 1;
    g_rooms[1].neighbours[0] = 0; g_rooms[1].neighbours[1] = 2; g_rooms[1].neighbour_count = 2;
    g_rooms[2].neighbours[0] = 1; g_rooms[2].neighbour_count = 1;

    int door_index = 0;
    for (int i = 0; i < ROOM_COUNT; i++) {
        char path[64]; snprintf(path, sizeof(path), "rom:/maps/fps_room%d.map", i);
        FigMap tmp;
        if (fig_map_load(&tmp, path) < 0) { debugf("fps: %s did not load\n", path); continue; }
        int n = 0;
        for (int j = 0; j < tmp.spawn_count && n < FIG_ROOM_MAX_SPAWNS; j++) {
            FigRoomSpawn *sp = &tmp.spawns[j];
            if (sp->profile_id == PROFILE_START) {
                g_spawn_pos = sp->pos;
                /* Quake's angle is degrees from +X; fig_fpscam's yaw is from +Z. */
                g_spawn_yaw = 1.5708f - sp->yaw;
                continue;
            }
            if (sp->profile_id == PROFILE_TRIGGER) {
                FigTrigger t = { .active = 1 };
                sscanf(fig_dict_get_str(&sp->dict, "mins", "0 0 0"), "%f %f %f", &t.mins.v[0], &t.mins.v[1], &t.mins.v[2]);
                sscanf(fig_dict_get_str(&sp->dict, "maxs", "0 0 0"), "%f %f %f", &t.maxs.v[0], &t.maxs.v[1], &t.maxs.v[2]);
                t.event_id = (uint16_t)fig_dict_get_int(&sp->dict, "event_id", 0);
                t.type = (uint8_t)fig_dict_get_int(&sp->dict, "type", FIG_TRIG_ONCE);
                fig_trigger_add(&t);
                continue;
            }
            if (sp->profile_id == PROFILE_KEY_DOOR) {
                char idx[12]; snprintf(idx, sizeof idx, "%d", door_index++);
                fig_dict_set_auto(&sp->dict, "door_index", idx);
            }
            g_rooms[i].spawns[n++] = *sp;
        }
        g_rooms[i].spawn_count = (uint8_t)n;
        fig_map_free(&tmp);
    }

    fig_room_system_init(&g_room_sys, g_rooms, ROOM_COUNT, 3,
        on_room_load, on_room_unload, on_room_spawn, on_room_draw, NULL,
        /*owns_clip_world=*/0);

    fig_fpscam_init(&g_fpscam);
    /* A box standing on the floor with the eye near its top: fig_fpscam puts
     * the eye at `pos`. Its default box is 48 deep along Z and 16 tall. */
    g_fpscam.mins = (fm_vec3_t){{ -10, -38, -10 }};
    g_fpscam.maxs = (fm_vec3_t){{  10,   4,  10 }};
    g_fpscam.move_speed = 110;
    g_fpscam.run_speed = 170;
    respawn();

    FigActorHandle player = fig_actor_spawn(PROFILE_PLAYER, g_spawn_pos, g_spawn_yaw, NULL);

    if (KILN_JUMP == JUMP_SWITCH) {
        /* In the corridor, beside the switch, facing it. */
        fig_fpscam_snap(&g_fpscam, (fm_vec3_t){{ -18, 40, 184 }}, fm_atan2f(-22, 12), -0.35f);
        fig_input_play(1, &SWITCH_TAPE);
    }
    if (KILN_JUMP == JUMP_COMBAT) {
        /* Down the hall from the spawn, pitched so the shot meets a grunt. */
        fig_fpscam_snap(&g_fpscam, (fm_vec3_t){{ 0, 40, -150 }}, 0, -0.41f);
        fig_input_play(1, &COMBAT_TAPE);
    }
    if (KILN_JUMP == JUMP_DOOR) {
        fig_fpscam_snap(&g_fpscam, (fm_vec3_t){{ 0, 40, 450 }}, 0, 0.25f);
        fig_input_play(1, &DOOR_TAPE);
    }
    if (KILN_JUMP == JUMP_NONE) fig_input_set_attract(1, &ATTRACT_TAPE, 120);

    fig_weapons_init(&g_wset, WEAPON_DEFS, 4);
    fig_scene_init(&g_scene);
    g_scene.far_z = 700; g_scene.near_z = 4; g_scene.fov_deg = 70;
    fig_prim_stage(&g_scene, RGBA32(0x18, 0x14, 0x1C, 0xFF), 220.0f, 680.0f);

    build_models();

    uint32_t frames = 0; float fps = 0; uint32_t last_ticks = get_ticks(); float ta = 0;
    int jump_ready = 0;

    for (;;) {
        fig_input_update();
        const FigInput *in = fig_input_get(1);
        float dt = 1.0f/60.0f; ta += dt;

        fig_room_system_update(&g_room_sys, g_fpscam.pos);
        if (!jump_ready) { jump_setup(); jump_ready = 1; }
        jump_latch();
        compose_world();

        int dialogue_active = fig_dialogue_active(&g_dialogue);
        const int playing = !dialogue_active && !g_won;

        if (playing) {
            g_level_time += dt;
            fig_fpscam_update(&g_fpscam, in, dt);
            fig_weapons_update(&g_wset, dt);
        }
        if (g_won && (in->edges & FIG_BTN_START)) {
            g_won = 0; g_level_time = 0; g_score = 0;
            respawn();
        }

        /* ── Z-targeting: lock, and turn the view onto the target ─────── */
        if (playing && (in->buttons & FIG_BTN_Z)) {
            if (!fig_actor_resolve(g_z_target))
                g_z_target = fig_target_acquire(g_fpscam.pos, fig_fpscam_forward(&g_fpscam), 0.5f, 300);
            FigActor *t = fig_actor_resolve(g_z_target);
            if (t) {
                const float dx = t->xform.pos.v[0] - g_fpscam.pos.v[0];
                const float dz = t->xform.pos.v[2] - g_fpscam.pos.v[2];
                float want = fm_atan2f(dx, dz) - g_fpscam.yaw;
                while (want >  3.14159f) want -= 6.28318f;
                while (want < -3.14159f) want += 6.28318f;
                g_fpscam.yaw += want * 0.2f;
            }
        } else {
            g_z_target = FIG_ACTOR_HANDLE_NONE;
        }

        if (playing) {
            if (in->edges & FIG_BTN_DL) fig_weapons_prev(&g_wset);
            if (in->edges & FIG_BTN_DR) fig_weapons_next(&g_wset);
            if (in->edges & FIG_BTN_CD) fig_weapons_reload(&g_wset);
        }

        /* ── Context action first; otherwise A fires ─────────────────── */
        /* Scanned from chest height, not the eye: fig_context's cone test
         * divides the horizontal dot by the 3D distance, so from 20 units above
         * a waist-high switch the switch fell outside the cone while the player
         * was looking straight at it, and A fired instead. */
        const fm_vec3_t chest = {{ g_fpscam.pos.v[0], g_fpscam.pos.v[1] - 22, g_fpscam.pos.v[2] }};
        g_ctx_action = playing ? fig_context_scan(chest, g_fpscam.yaw, 44, 0.6f, &g_ctx_actor)
                               : FIG_CTX_NONE;
        if (playing && g_ctx_action != FIG_CTX_NONE && (in->edges & FIG_BTN_A)) {
            FigActor *a = fig_actor_resolve(g_ctx_actor);
            switch (g_ctx_action) {
            case FIG_CTX_TALK:
                if (a) {
                    NpcState *n = (void *)a->state;
                    const char *lines[] = { n->line };
                    fig_dialogue_start(&g_dialogue, lines, 1);
                    fig_sound_play("npc_talk", a->xform.pos, 1);
                }
                break;
            case FIG_CTX_OPEN:     /* the gate: health 0, no key */
                if (a && a->profile_id == PROFILE_KEY_DOOR) {
                    fig_sound_play("door_locked", a->xform.pos, 1);
                    toast("this gate opens from a switch");
                }
                break;
            case FIG_CTX_UNLOCK:
                if (a && a->profile_id == PROFILE_KEY_DOOR) {
                    DoorState *d = (void *)a->state;
                    if (fig_inventory_has(&g_inv, d->key_id)) {
                        fig_inventory_consume(&g_inv, d->key_id, 1);
                        fig_event_post(g_ctx_actor, EV_DOOR_OPEN, 0, NULL, 0, 1);
                    } else {
                        fig_sound_play("door_locked", a->xform.pos, 1);
                        toast("locked: it needs the red key");
                    }
                }
                break;
            case FIG_CTX_OPEN_CHEST:
                fig_event_post(g_ctx_actor, EV_CHEST_OPEN, 0, NULL, 0, 1);
                break;
            case FIG_CTX_USE:
                if (a && a->profile_id == PROFILE_SWITCH) {
                    SwitchState *sw = (void *)a->state;
                    /* Resolved when pressed, not at spawn: the door lives in
                     * whichever room owns it and exists only while loaded. */
                    FigActor *door = find_door(sw->target_door);
                    if (!sw->activated && door) {
                        sw->activated = 1;
                        fig_event_post(fig_actor_handle_of(door), EV_DOOR_OPEN, 400, NULL, 0, 1);
                        toast("the gate rises");
                    }
                }
                break;
            default: break;
            }
        } else if (playing && (in->buttons & FIG_BTN_A) && g_ctx_action == FIG_CTX_NONE) {
            fire_weapon();
        }

        /* ── Trigger volumes post to the player actor ─────────────────── */
        fm_vec3_t push_vel = {{ 0, 0, 0 }};
        if (fig_actor_resolve(player)) {
            fig_trigger_update(g_fpscam.pos, player, dt, &push_vel);
            g_fpscam.pos.v[0] += push_vel.v[0] * dt;
            g_fpscam.pos.v[2] += push_vel.v[2] * dt;
        }

        fig_projectile_update(dt);
        fig_event_process(dt);
        fig_actor_update_all(dt);
        if (dialogue_active) fig_dialogue_update(&g_dialogue, dt, in);

        if (g_player_health <= 0) { toast("you died"); respawn(); }

        if (g_muzzle_flash>0) g_muzzle_flash-=dt;
        if (g_hitmarker>0) g_hitmarker-=dt;
        if (g_damage_flash>0) g_damage_flash-=dt;
        if (g_shake_t>0) g_shake_t-=dt;
        if (g_toast_t>0) g_toast_t-=dt;

        fig_fpscam_apply(&g_fpscam, &g_scene);
        if (g_shake_t > 0) {
            float sh = g_shake_mag * (g_shake_t / 0.25f);
            g_scene.cam_pos.v[0] += fm_sinf(ta*40)*sh;
            g_scene.cam_pos.v[1] += fm_cosf(ta*37)*sh;
        }
        fig_scene_update(&g_scene);
        fig_sound_update_listener(g_fpscam.pos, fig_fpscam_forward(&g_fpscam));

        if (++frames % 30 == 0) {
            uint32_t now = get_ticks();
            fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND);
            last_ticks = now;
        }

        /* ── 3D ───────────────────────────────────────────── */
        fig_frame_begin();
        fig_scene_begin(&g_scene);
        fig_room_draw_all(&g_room_sys);
        fig_actor_draw_all();
        fig_projectile_draw_all();
        if (!g_won) draw_viewmodel();

        /* ── 2D ───────────────────────────────────────────── */
        fig_gui_begin();
        const color_t ink = RGBA32(232,232,240,255), teal = RGBA32(0,245,212,255);

        /* Damage: a red frame, not a full-screen translucent fill — the RDP
         * ignores source alpha with the blender off, so that was opaque red. */
        if (g_damage_flash > 0) {
            const color_t red = RGBA32(200, 20, 20, 255);
            fig_gui_rect(0, 0, SCREEN_W, 6, red); fig_gui_rect(0, SCREEN_H-6, SCREEN_W, 6, red);
            fig_gui_rect(0, 0, 6, SCREEN_H, red); fig_gui_rect(SCREEN_W-6, 0, 6, SCREEN_H, red);
        }

        { int cx=SCREEN_W/2, cy=SCREEN_H/2;
        fig_gui_rect(cx-1,cy-6,2,5,teal); fig_gui_rect(cx-1,cy+2,2,5,teal);
        fig_gui_rect(cx-6,cy-1,5,2,teal); fig_gui_rect(cx+2,cy-1,5,2,teal);
        if (g_hitmarker>0) { color_t h=RGBA32(255,255,255,255);
            fig_gui_rect(cx-8,cy-8,4,1,h); fig_gui_rect(cx+5,cy-8,4,1,h);
            fig_gui_rect(cx-8,cy+7,4,1,h); fig_gui_rect(cx+5,cy+7,4,1,h); }
        if (g_muzzle_flash>0) { color_t m=RGBA32(255,220,80,255);
            fig_gui_rect(cx-3,cy-1,7,2,m); fig_gui_rect(cx-1,cy-3,2,7,m); } }

        FigActor *zt = fig_actor_resolve(g_z_target);
        if (zt) fig_target_draw_reticle(&g_scene, zt->xform.pos, SCREEN_W, SCREEN_H, RGBA32(255,200,0,255));

        if (g_ctx_action != FIG_CTX_NONE)
            fig_gui_text(SCREEN_W/2 - 24, SCREEN_H/2 + 22, RGBA32(255,255,0,255), "A %s", fig_context_label(g_ctx_action));
        if (g_toast_t > 0 && g_toast)
            fig_gui_text(SCREEN_W/2 - (int)strlen(g_toast) * 3, SCREEN_H/2 - 30, RGBA32(255,255,255,255), "%s", g_toast);

        /* 206 wide: the objective line is up to 33 characters (6 px each) and
         * ran past a 150-wide panel on console; the HP bars start at x 222. */
        fig_gui_panel(8, 8, 206, 52, RGBA32(10,10,24,255), teal);
        const FigWeaponDef *wd = fig_weapons_active_def(&g_wset);
        FigWeapon *ws = fig_weapons_active_state(&g_wset);
        fig_gui_text(14, 20, teal, "KILN FPS");
        fig_gui_text(70, 20, RGBA32(144,152,176,255), "%4.1f fps", fps);
        fig_gui_text(14, 32, ink, "score %d  enemies %d", g_score, fig_actor_count(FIG_ACTOR_CAT_ENEMY));
        fig_gui_text(14, 44, ink, "%s %d/%d%s", wd ? wd->name : "?", ws ? ws->magazine : 0, ws ? ws->ammo : 0,
                      ws && ws->state == FIG_WPN_RELOADING ? " reload" : "");
        fig_gui_text(14, 56, RGBA32(255,210,90,255), "%s", objective());

        int hb_w=90, hb_x=SCREEN_W-hb_w-8;
        fig_gui_text(hb_x, 14, ink, "HP");
        fig_gui_bar(hb_x+18, 8, hb_w-18, 8, (float)g_player_health/100, RGBA32(200,30,30,255), RGBA32(40,40,40,255));
        fig_gui_text(hb_x, 28, ink, "AR");
        fig_gui_bar(hb_x+18, 22, hb_w-18, 8, (float)g_player_armor/100, RGBA32(68,136,255,255), RGBA32(40,40,40,255));
        if (fig_inventory_has(&g_inv, ITEM_KEY_RED)) fig_gui_text(hb_x, 42, RGBA32(255,80,80,255), "RED KEY");
        /* Under the bars, not bottom-right: the gun lives there. */
        if (fig_input_scripted(1)) {
            fig_gui_panel(SCREEN_W - 58, 50, 50, 16, RGBA32(0xC0, 0x30, 0x60, 0xFF), RGBA32(0xFF, 0xFF, 0xFF, 0xFF));
            fig_gui_text(SCREEN_W - 49, 62, RGBA32(0xFF, 0xFF, 0xFF, 0xFF), "DEMO");
        }

        if (dialogue_active) fig_dialogue_draw(&g_dialogue);

        if (g_won) {
            fig_gui_panel(60, 90, 200, 60, RGBA32(10,10,24,255), RGBA32(255,210,90,255));
            fig_gui_text(106, 108, RGBA32(255,210,90,255), "LEVEL COMPLETE");
            fig_gui_text(84, 124, ink, "score %d   time %.1f s", g_score, g_level_time);
            fig_gui_text(96, 140, RGBA32(144,152,176,255), "START to play again");
        } else {
            fig_gui_text(8, SCREEN_H-18, RGBA32(139,92,246,255), "stick move  C look  A fire/act  Z lock");
            fig_gui_text(8, SCREEN_H-6,  RGBA32(139,92,246,255), "B jump  R run  D< D> weapon  C-down reload");
        }

        fig_gui_end();
        fig_frame_end();

        fig_sound_update();
        fig_audio_update();
    }
}
