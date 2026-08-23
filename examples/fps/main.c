// SPDX-License-Identifier: MIT
//
// FPS with integrated Half-Life, Doom, and Ocarina of Time mechanics.
//
//   Doom:        weapon switching (pistol/shotgun/rocket/plasma),
//                projectiles, armor, keycard-locked doors, barrels
//   Half-Life:   physics props, trigger volumes, NPC dialogue,
//                HEV-suit-style health+armor
//   OoT:         Z-targeting, context-action button, chests, NPC talk,
//                key-lock doors, switch→door event chains
//
// Engine modules used:
//   kiln_fpscam    kiln_weapons  kiln_clip      kiln_map      kiln_room
//   kiln_actor     kiln_event    kiln_surface  kiln_sound    kiln_audio
//   kiln_gui       kiln_target  kiln_inventory  kiln_projectile
//   kiln_trigger   kiln_context kiln_dialogue

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

#include <malloc.h>
#include <string.h>

#define SCREEN_W 320
#define SCREEN_H 240
#define ACTOR_POOL_CAP 48
#define ROOM_COUNT 3

// ── Item IDs ───────────────────────────────────────────────────────────
enum {
    ITEM_KEY_RED = 1,
    ITEM_ARMOR = 10,
};

// ── Event IDs ──────────────────────────────────────────────────────────
enum {
    EV_ENEMY_ATTACK = 0x20,
    EV_DOOR_OPEN    = 0x30,
    EV_CHEST_OPEN   = 0x40,
    EV_AMBUSH       = 0x101,
};

// ── Profiles ──────────────────────────────────────────────────────────
enum {
    PROFILE_GRUNT, PROFILE_HEAVY, PROFILE_HEALTH_PICKUP, PROFILE_AMMO_PICKUP,
    PROFILE_ARMOR_PICKUP, PROFILE_KEY_PICKUP, PROFILE_NPC,
    PROFILE_CHEST, PROFILE_KEY_DOOR, PROFILE_SWITCH, PROFILE_BARREL,
    PROFILE_COUNT
};

// ── Cube helper ─────────────────────────────────────────────────────────
static const uint8_t CUBE_TRIS[12][3] = {
    {0,1,2},{2,3,0}, {4,6,5},{6,4,7},
    {0,4,5},{5,1,0}, {1,5,6},{6,2,1},
    {2,6,7},{7,3,2}, {3,7,4},{4,0,3},
};

static T3DVertPacked *make_color_cube(int16_t half, uint32_t rgba)
{
    T3DVertPacked *v = malloc_uncached(sizeof(T3DVertPacked) * 4);
    const int16_t s = half;
    const int16_t c[8][3] = {
        {-s,-s,-s},{ s,-s,-s},{ s, s,-s},{-s, s,-s},
        {-s,-s, s},{ s,-s, s},{ s, s, s},{-s, s, s},
    };
    for (int i = 0; i < 8; i += 2) {
        fm_vec3_t na = {{ (float)c[i][0],   (float)c[i][1],   (float)c[i][2]   }};
        fm_vec3_t nb = {{ (float)c[i+1][0], (float)c[i+1][1], (float)c[i+1][2] }};
        fm_vec3_norm(&na, &na);
        fm_vec3_norm(&nb, &nb);
        v[i / 2] = (T3DVertPacked){
            .posA = { c[i][0],   c[i][1],   c[i][2]   }, .rgbaA = rgba,
            .normA = t3d_vert_pack_normal(&na),
            .posB = { c[i+1][0], c[i+1][1], c[i+1][2] }, .rgbaB = rgba,
            .normB = t3d_vert_pack_normal(&nb),
        };
    }
    return v;
}

static void draw_cube(T3DVertPacked *v)
{
    t3d_vert_load(v, 0, 8);
    for (int i = 0; i < 12; i++)
        t3d_tri_draw(CUBE_TRIS[i][0], CUBE_TRIS[i][1], CUBE_TRIS[i][2]);
    t3d_tri_sync();
}

static T3DVertPacked *g_cube_grunt, *g_cube_heavy, *g_cube_health, *g_cube_ammo;
static T3DVertPacked *g_cube_armor, *g_cube_key, *g_cube_npc;
static T3DVertPacked *g_cube_chest_c, *g_cube_chest_o;
static T3DVertPacked *g_cube_door_c, *g_cube_door_o;
static T3DVertPacked *g_cube_switch, *g_cube_barrel;

// ── Globals ────────────────────────────────────────────────────────────
static KilnFpsCam g_fpscam;
static KilnWeaponSet g_wset;
static KilnInventory g_inv;
static KilnDialogue g_dialogue;
static KilnScene g_scene;
static int g_player_health = 100;
static int g_player_armor = 0;
static int g_score = 0;
static fm_vec3_t g_spawn_pos;
static float g_spawn_yaw;

static KilnActorHandle g_z_target = KILN_ACTOR_HANDLE_NONE;
static KilnContextAction g_ctx_action = KILN_CTX_NONE;
static KilnActorHandle g_ctx_actor = KILN_ACTOR_HANDLE_NONE;

static float g_muzzle_flash = 0, g_hitmarker = 0, g_damage_flash = 0;
static float g_shake_t = 0, g_shake_mag = 0;

// ── Enemy ──────────────────────────────────────────────────────────────
enum { ENEMY_IDLE, ENEMY_CHASE, ENEMY_ATTACK_S, ENEMY_FLEE };
typedef struct { float speed, attack_cd, scan_cd; uint8_t state; int flee_thresh, damage; } EnemyState;

static void enemy_init(KilnActor *self, const KilnDict *args)
{
    EnemyState *s = (void *)self->state;
    if (self->profile_id == PROFILE_HEAVY) {
        s->speed = 15; s->damage = 15; s->flee_thresh = 0; self->health = 200;
    } else {
        s->speed = 30; s->damage = 8; s->flee_thresh = 30; self->health = 100;
    }
    s->attack_cd = 0; s->scan_cd = 0; s->state = ENEMY_IDLE;
    (void)args;
}

static int has_los(fm_vec3_t from, fm_vec3_t to)
{
    fm_vec3_t dir = {{ to.v[0]-from.v[0], to.v[1]-from.v[1], to.v[2]-from.v[2] }};
    float d = fm_vec3_len(&dir);
    if (d < 0.1f) return 1;
    dir.v[0]/=d; dir.v[1]/=d; dir.v[2]/=d;
    fm_vec3_t end = {{ from.v[0]+dir.v[0]*d, from.v[1]+dir.v[1]*d, from.v[2]+dir.v[2]*d }};
    KilnTrace tr = kiln_clip_ray(from, end);
    return tr.fraction >= 1.0f;
}

static void enemy_update(KilnActor *self, float dt)
{
    EnemyState *s = (void *)self->state;
    fm_vec3_t tp = {{ g_fpscam.pos.v[0]-self->xform.pos.v[0], 0, g_fpscam.pos.v[2]-self->xform.pos.v[2] }};
    float d = fm_vec3_len(&tp);
    s->scan_cd -= dt;
    if (s->scan_cd <= 0) {
        s->scan_cd = 0.5f;
        fm_vec3_t eye = {{ self->xform.pos.v[0], self->xform.pos.v[1]+8, self->xform.pos.v[2] }};
        if (has_los(eye, g_fpscam.pos)) s->state = (d < 20) ? ENEMY_ATTACK_S : ENEMY_CHASE;
        else s->state = ENEMY_IDLE;
        if (self->health < s->flee_thresh && s->flee_thresh > 0) s->state = ENEMY_FLEE;
    }
    s->attack_cd -= dt;
    switch (s->state) {
    case ENEMY_CHASE:
        if (d > 1) {
            fm_vec3_t dir = {{ tp.v[0]/d, 0, tp.v[2]/d }};
            fm_vec3_t vel = {{ dir.v[0]*s->speed*dt, 0, dir.v[2]*s->speed*dt }};
            self->xform.pos = kiln_clip_slide(self->xform.pos, vel,
                (fm_vec3_t){{-8,-8,-8}}, (fm_vec3_t){{8,8,8}}, 4);
        }
        break;
    case ENEMY_ATTACK_S:
        if (s->attack_cd <= 0) {
            s->attack_cd = 0.8f;
            int32_t a[1] = { s->damage };
            kiln_event_post(kiln_actor_handle_of(self), EV_ENEMY_ATTACK, 200, a, 1, 2);
        }
        break;
    case ENEMY_FLEE:
        if (d > 0.1f) {
            fm_vec3_t dir = {{ -tp.v[0]/d, 0, -tp.v[2]/d }};
            fm_vec3_t vel = {{ dir.v[0]*s->speed*dt, 0, dir.v[2]*s->speed*dt }};
            self->xform.pos = kiln_clip_slide(self->xform.pos, vel,
                (fm_vec3_t){{-8,-8,-8}}, (fm_vec3_t){{8,8,8}}, 4);
        }
        break;
    }
    if (s->state != ENEMY_FLEE) self->xform.rot_angle = fm_atan2f(tp.v[0], tp.v[2]);
    else self->xform.rot_angle = fm_atan2f(-tp.v[0], -tp.v[2]);
}

static void enemy_event(KilnActor *self, uint16_t eid, const int32_t *a, uint8_t c)
{
    if (eid == EV_ENEMY_ATTACK && c >= 1) {
        fm_vec3_t tp = {{ g_fpscam.pos.v[0]-self->xform.pos.v[0], 0, g_fpscam.pos.v[2]-self->xform.pos.v[2] }};
        if (fm_vec3_len(&tp) < 25) {
            int dmg = a[0];
            if (g_player_armor > 0) { g_player_armor -= dmg/2; g_player_health -= dmg - dmg/2; if (g_player_armor<0) g_player_armor=0; }
            else g_player_health -= dmg;
            g_damage_flash = 0.3f; g_shake_t = 0.25f; g_shake_mag = 4;
        }
    }
}

static void grunt_draw(KilnActor *s){(void)s;draw_cube(g_cube_grunt);}
static void heavy_draw(KilnActor *s){(void)s;draw_cube(g_cube_heavy);}

// ── Pickups ─────────────────────────────────────────────────────────────
typedef struct { float bob_t; fm_vec3_t home; } PickupState;
static void pickup_init(KilnActor *s, const KilnDict *a){PickupState*p=(void*)s->state;p->bob_t=0;p->home=s->xform.pos;(void)a;}
static void pickup_bob(KilnActor *s, float dt){PickupState*p=(void*)s->state;p->bob_t+=dt;s->xform.pos.v[1]=p->home.v[1]+fm_sinf(p->bob_t*3)*2;s->xform.rot_angle=p->bob_t*1.5f;}

static void health_update(KilnActor *s, float dt){
    pickup_bob(s,dt);
    fm_vec3_t tp={{g_fpscam.pos.v[0]-s->xform.pos.v[0],0,g_fpscam.pos.v[2]-s->xform.pos.v[2]}};
    if(fm_vec3_len(&tp)<15){g_player_health+=25;if(g_player_health>100)g_player_health=100;kiln_sound_play("pickup",s->xform.pos,1);kiln_actor_despawn(kiln_actor_handle_of(s));}
}
static void ammo_update(KilnActor *s, float dt){
    pickup_bob(s,dt);
    fm_vec3_t tp={{g_fpscam.pos.v[0]-s->xform.pos.v[0],0,g_fpscam.pos.v[2]-s->xform.pos.v[2]}};
    if(fm_vec3_len(&tp)<15){KilnWeapon*w=kiln_weapons_active_state(&g_wset);w->ammo+=24;kiln_sound_play("pickup",s->xform.pos,1);kiln_actor_despawn(kiln_actor_handle_of(s));}
}
static void armor_update(KilnActor *s, float dt){
    pickup_bob(s,dt);
    fm_vec3_t tp={{g_fpscam.pos.v[0]-s->xform.pos.v[0],0,g_fpscam.pos.v[2]-s->xform.pos.v[2]}};
    if(fm_vec3_len(&tp)<15){g_player_armor+=25;if(g_player_armor>100)g_player_armor=0;kiln_sound_play("pickup",s->xform.pos,1);kiln_actor_despawn(kiln_actor_handle_of(s));}
}
static void key_update(KilnActor *s, float dt){
    pickup_bob(s,dt);
    fm_vec3_t tp={{g_fpscam.pos.v[0]-s->xform.pos.v[0],0,g_fpscam.pos.v[2]-s->xform.pos.v[2]}};
    if(fm_vec3_len(&tp)<15){kiln_inventory_add(&g_inv,ITEM_KEY_RED,1);kiln_sound_play("pickup",s->xform.pos,1);kiln_actor_despawn(kiln_actor_handle_of(s));}
}
static void health_draw(KilnActor*s){(void)s;draw_cube(g_cube_health);}
static void ammo_draw(KilnActor*s){(void)s;draw_cube(g_cube_ammo);}
static void armor_draw(KilnActor*s){(void)s;draw_cube(g_cube_armor);}
static void key_draw(KilnActor*s){(void)s;draw_cube(g_cube_key);}

// ── NPC ────────────────────────────────────────────────────────────────
typedef struct { const char *line; uint8_t talked; } NpcState;
static void npc_init(KilnActor *s, const KilnDict *a){NpcState*n=(void*)s->state;n->talked=0;n->line=kiln_dict_get_str(a,"dialogue","...");}
static void npc_update(KilnActor *s, float dt){(void)s;(void)dt;}
static void npc_draw(KilnActor *s){(void)s;draw_cube(g_cube_npc);}

// ── Chest (OoT) ────────────────────────────────────────────────────────
typedef struct { uint8_t open; int contents; } ChestState;
static void chest_init(KilnActor *s, const KilnDict *a){ChestState*c=(void*)s->state;c->open=0;c->contents=kiln_dict_get_int(a,"contents",0);}
static void chest_update(KilnActor *s, float dt){(void)s;(void)dt;}
static void chest_draw(KilnActor *s){ChestState*c=(void*)s->state;draw_cube(c->open?g_cube_chest_o:g_cube_chest_c);}
static void chest_event(KilnActor *s, uint16_t eid, const int32_t*a, uint8_t c){
    (void)a;(void)c;
    if(eid==EV_CHEST_OPEN){ChestState*cs=(void*)s->state;cs->open=1;kiln_sound_play("chest_open",s->xform.pos,1);
        if(cs->contents==ITEM_KEY_RED){KilnActorHandle h=kiln_actor_spawn(PROFILE_KEY_PICKUP,s->xform.pos,s->xform.rot_angle,NULL);KilnActor*k=kiln_actor_resolve(h);if(k)k->xform.pos.v[1]+=10;}
    }
}

// ── Key Door (Doom/OoT) ────────────────────────────────────────────────
typedef struct { float cur, target; uint8_t open; int key_id; } DoorState;
static void door_init(KilnActor *s, const KilnDict *a){DoorState*d=(void*)s->state;d->cur=0;d->target=0;d->open=0;d->key_id=kiln_dict_get_int(a,"key_id",0);s->health=d->key_id;}
static void door_update(KilnActor *s, float dt){DoorState*d=(void*)s->state;float t=4*dt;if(t>1)t=1;d->cur+=(d->target-d->cur)*t;s->xform.rot_angle=d->cur;}
static void door_draw(KilnActor *s){DoorState*d=(void*)s->state;draw_cube(d->open?g_cube_door_o:g_cube_door_c);}
static void door_event(KilnActor *s, uint16_t eid, const int32_t*a, uint8_t c){
    (void)a;(void)c;
    if(eid==EV_DOOR_OPEN){DoorState*d=(void*)s->state;d->open=1;d->target=1.5708f;s->health=0;kiln_sound_play("door_open",s->xform.pos,1);}
}

// ── Switch (OoT) ───────────────────────────────────────────────────────
typedef struct { KilnActorHandle door; uint8_t activated; } SwitchState;
static void switch_init(KilnActor *s, const KilnDict *a){SwitchState*sw=(void*)s->state;sw->door=KILN_ACTOR_HANDLE_NONE;sw->activated=0;(void)a;}
static void switch_update(KilnActor *s, float dt){(void)s;(void)dt;}
static void switch_draw(KilnActor *s){(void)s;draw_cube(g_cube_switch);}

// ── Barrel (Half-Life) ─────────────────────────────────────────────────
typedef struct { float hp; } BarrelState;
static void barrel_init(KilnActor *s, const KilnDict *a){BarrelState*b=(void*)s->state;b->hp=30;s->health=30;(void)a;}
static void barrel_update(KilnActor *s, float dt){(void)s;(void)dt;}
static void barrel_draw(KilnActor *s){(void)s;draw_cube(g_cube_barrel);}
static void barrel_event(KilnActor *s, uint16_t eid, const int32_t *a, uint8_t c){
    (void)c;(void)a;
    if(eid==EV_ENEMY_ATTACK){BarrelState*b=(void*)s->state;b->hp-=10;s->health=b->hp;if(b->hp<=0){
        kiln_sound_play("explosion",s->xform.pos,1);
        for(KilnActor*e=kiln_actor_first(KILN_ACTOR_CAT_ENEMY);e;e=kiln_actor_next(e)){
            fm_vec3_t d={{e->xform.pos.v[0]-s->xform.pos.v[0],0,e->xform.pos.v[2]-s->xform.pos.v[2]}};
            if(fm_vec3_len(&d)<40){e->health-=50;}
        }
        kiln_actor_despawn(kiln_actor_handle_of(s));
    }}
}

static const KilnActorProfile PROFILES[PROFILE_COUNT] = {
    [PROFILE_GRUNT]  = { .name="grunt", .category=KILN_ACTOR_CAT_ENEMY, .state_size=sizeof(EnemyState), .init=enemy_init, .update=enemy_update, .draw=grunt_draw, .event=enemy_event },
    [PROFILE_HEAVY]  = { .name="heavy", .category=KILN_ACTOR_CAT_ENEMY, .state_size=sizeof(EnemyState), .init=enemy_init, .update=enemy_update, .draw=heavy_draw, .event=enemy_event },
    [PROFILE_HEALTH_PICKUP] = { .name="health", .category=KILN_ACTOR_CAT_ITEM, .state_size=sizeof(PickupState), .init=pickup_init, .update=health_update, .draw=health_draw },
    [PROFILE_AMMO_PICKUP]   = { .name="ammo", .category=KILN_ACTOR_CAT_ITEM, .state_size=sizeof(PickupState), .init=pickup_init, .update=ammo_update, .draw=ammo_draw },
    [PROFILE_ARMOR_PICKUP]  = { .name="armor", .category=KILN_ACTOR_CAT_ITEM, .state_size=sizeof(PickupState), .init=pickup_init, .update=armor_update, .draw=armor_draw },
    [PROFILE_KEY_PICKUP]    = { .name="key", .category=KILN_ACTOR_CAT_ITEM, .state_size=sizeof(PickupState), .init=pickup_init, .update=key_update, .draw=key_draw },
    [PROFILE_NPC]           = { .name="npc", .category=KILN_ACTOR_CAT_NPC, .state_size=sizeof(NpcState), .init=npc_init, .update=npc_update, .draw=npc_draw },
    [PROFILE_CHEST]         = { .name="chest", .category=KILN_ACTOR_CAT_CHEST, .state_size=sizeof(ChestState), .init=chest_init, .update=chest_update, .draw=chest_draw, .event=chest_event },
    [PROFILE_KEY_DOOR]      = { .name="keydoor", .category=KILN_ACTOR_CAT_DOOR, .state_size=sizeof(DoorState), .init=door_init, .update=door_update, .draw=door_draw, .event=door_event },
    [PROFILE_SWITCH]        = { .name="switch", .category=KILN_ACTOR_CAT_PROP, .state_size=sizeof(SwitchState), .init=switch_init, .update=switch_update, .draw=switch_draw },
    [PROFILE_BARREL]        = { .name="barrel", .category=KILN_ACTOR_CAT_PROP, .state_size=sizeof(BarrelState), .init=barrel_init, .update=barrel_update, .draw=barrel_draw, .event=barrel_event },
};

static KilnActor g_pool[ACTOR_POOL_CAP];

// ── Room streaming ─────────────────────────────────────────────────────
static KilnRoom g_rooms[ROOM_COUNT];
static KilnRoomSystem g_room_sys;
static KilnMap g_room_maps[ROOM_COUNT];
static int g_room_loaded[ROOM_COUNT];
static KilnActorHandle g_door_handles[ROOM_COUNT * 4];

static void on_room_load(KilnRoom *room, void *user)
{
    (void)user;
    if (g_room_loaded[room->id]) return;
    char path[64];
    snprintf(path, sizeof(path), "rom:/maps/fps_room%d.map", room->id);
    if (kiln_map_load(&g_room_maps[room->id], path) < 0) return;
    room->brushes = g_room_maps[room->id].brushes;
    room->brush_count = g_room_maps[room->id].brush_count;
    room->user_mesh = &g_room_maps[room->id];
    g_room_loaded[room->id] = 1;
}
static void on_room_unload(KilnRoom *room, void *user)
{
    (void)user;
    if (!g_room_loaded[room->id]) return;
    kiln_map_free(&g_room_maps[room->id]);
    room->brushes = NULL; room->brush_count = 0; room->user_mesh = NULL;
    g_room_loaded[room->id] = 0;
}
static void on_room_spawn(KilnRoom *room, const KilnRoomSpawn *spawn, void *user)
{
    (void)room; (void)user;
    kiln_actor_spawn_in_room(spawn->profile_id, spawn->pos, spawn->yaw, room->id, &spawn->dict);
}
static void on_room_draw(KilnRoom *room, void *user)
{
    (void)user;
    if (room->user_mesh) kiln_map_draw((const KilnMap *)room->user_mesh);
}

// ── Weapon definitions ─────────────────────────────────────────────────
static const KilnWeaponDef WEAPON_DEFS[] = {
    { "Pistol",  KILN_WTYPE_HITSCAN,    0, 12, 0.15f, 1.5f, 34, 0,   "gunshot",      0xFFD94CFF },
    { "Shotgun", KILN_WTYPE_HITSCAN,    0, 6,  0.6f,  2.0f, 20, 0,   "shotgun_fire", 0xFF8844FF },
    { "Rocket",  KILN_WTYPE_PROJECTILE, KILN_PROJ_ROCKET_W, 1, 0.8f, 2.5f, 80, 40, "rocket_fire",  0xFF4444FF },
    { "Plasma",  KILN_WTYPE_PROJECTILE, KILN_PROJ_PLASMA_W, 20, 0.1f, 1.0f, 25, 0,  "plasma_fire",  0x44FFFFFF },
};

// ── Projectile hit callback ────────────────────────────────────────────
static void proj_hit_cb(uint16_t profile_id, fm_vec3_t pos, int damage, float radius)
{
    (void)radius;
    if (profile_id != 0xFFFF) {
        for (KilnActor *a = kiln_actor_first(KILN_ACTOR_CAT_ENEMY); a; a = kiln_actor_next(a)) {
            if (a->profile_id == profile_id || profile_id == 0) {
                fm_vec3_t d = {{ a->xform.pos.v[0]-pos.v[0], 0, a->xform.pos.v[2]-pos.v[2] }};
                if (fm_vec3_len(&d) < 20) { a->health -= damage; break; }
            }
        }
    } else {
        kiln_sound_play("impact", pos, 1);
    }
    for (KilnActor *a = kiln_actor_first(KILN_ACTOR_CAT_ENEMY); a; a = kiln_actor_next(a)) {
        fm_vec3_t d = {{ a->xform.pos.v[0]-pos.v[0], 0, a->xform.pos.v[2]-pos.v[2] }};
        if (fm_vec3_len(&d) < 20) {
            a->health -= damage;
            if (a->health <= 0) { kiln_actor_despawn(kiln_actor_handle_of(a)); g_score++; }
        }
    }
    for (KilnActor *b = kiln_actor_first(KILN_ACTOR_CAT_PROP); b; b = kiln_actor_next(b)) {
        if (b->profile_id == PROFILE_BARREL) {
            fm_vec3_t d = {{ b->xform.pos.v[0]-pos.v[0], 0, b->xform.pos.v[2]-pos.v[2] }};
            if (fm_vec3_len(&d) < 25) {
                int32_t args[1] = { 50 };
                kiln_event_post(kiln_actor_handle_of(b), EV_ENEMY_ATTACK, 0, args, 1, 3);
            }
        }
    }
}

// ── Hitscan ────────────────────────────────────────────────────────────
static int ray_hits_actor(fm_vec3_t o, fm_vec3_t d, KilnActor *a, float r)
{
    fm_vec3_t tc={{a->xform.pos.v[0]-o.v[0],a->xform.pos.v[1]-o.v[1],a->xform.pos.v[2]-o.v[2]}};
    float t=tc.v[0]*d.v[0]+tc.v[1]*d.v[1]+tc.v[2]*d.v[2];
    if(t<0)return 0;
    fm_vec3_t cp={{o.v[0]+d.v[0]*t,o.v[1]+d.v[1]*t,o.v[2]+d.v[2]*t}};
    float dx=a->xform.pos.v[0]-cp.v[0],dy=a->xform.pos.v[1]-cp.v[1],dz=a->xform.pos.v[2]-cp.v[2];
    return (dx*dx+dy*dy+dz*dz)<r*r;
}

static void do_hitscan(int damage)
{
    fm_vec3_t o = g_fpscam.pos;
    fm_vec3_t d = kiln_fpscam_forward(&g_fpscam);
    fm_vec3_t end = {{ o.v[0]+d.v[0]*500, o.v[1]+d.v[1]*500, o.v[2]+d.v[2]*500 }};
    KilnTrace tr = kiln_clip_ray(o, end);
    for (KilnActor *a = kiln_actor_first(KILN_ACTOR_CAT_ENEMY); a; a = kiln_actor_next(a)) {
        float r = (a->profile_id == PROFILE_HEAVY) ? 14 : 10;
        if (ray_hits_actor(o, d, a, r)) {
            a->health -= damage;
            if (a->health <= 0) { kiln_actor_despawn(kiln_actor_handle_of(a)); g_score++; }
            kiln_sound_play("enemy_hit", a->xform.pos, 1);
            g_hitmarker = 0.15f;
            return;
        }
    }
    for (KilnActor *b = kiln_actor_first(KILN_ACTOR_CAT_PROP); b; b = kiln_actor_next(b)) {
        if (b->profile_id == PROFILE_BARREL && ray_hits_actor(o, d, b, 12)) {
            int32_t args[1] = { 34 };
            kiln_event_post(kiln_actor_handle_of(b), EV_ENEMY_ATTACK, 0, args, 1, 3);
            return;
        }
    }
    if (tr.fraction < 1) {
        const KilnSurfaceDef *surf = kiln_surface_get(tr.hitsurface);
        if (surf && surf->footstep_sfx >= 0) kiln_sfx_play_ex(surf->footstep_sfx, -1, 1, 0.5f, 0.5f);
        else kiln_sound_play("impact", tr.endpos, 1);
    }
}

static void fire_weapon(void)
{
    if (!kiln_weapons_can_fire(&g_wset)) return;
    if (!kiln_weapons_fire(&g_wset)) return;
    const KilnWeaponDef *def = kiln_weapons_active_def(&g_wset);
    kiln_sound_play(def->sfx_name, g_fpscam.pos, 1);
    g_muzzle_flash = 0.05f;
    if (def->type == KILN_WTYPE_HITSCAN) {
        do_hitscan(def->damage);
    } else {
        fm_vec3_t fwd = kiln_fpscam_forward(&g_fpscam);
        fm_vec3_t vel = {{ fwd.v[0]*400, fwd.v[1]*400, fwd.v[2]*400 }};
        if (def->proj_type == KILN_PROJ_GRENADE_W) vel.v[1] += 100;
        kiln_projectile_spawn(def->proj_type, g_fpscam.pos, vel,
                              3.0f, def->damage, def->splash_radius);
    }
}

// ── Player event handler (for triggers) ────────────────────────────────
static KilnActor *g_player_actor;

static void player_event(KilnActor *self, uint16_t eid, const int32_t *a, uint8_t c)
{
    (void)self; (void)a; (void)c;
    if (eid == EV_AMBUSH) {
        /* Spawn two grunts behind the player. */
        fm_vec3_t p = {{ g_fpscam.pos.v[0] - fm_sinf(g_fpscam.yaw)*40,
                          g_fpscam.pos.v[1], g_fpscam.pos.v[2] - fm_cosf(g_fpscam.yaw)*40 }};
        kiln_actor_spawn(PROFILE_GRUNT, p, g_fpscam.yaw + 3.14f, NULL);
        fm_vec3_t p2 = {{ g_fpscam.pos.v[0] + fm_sinf(g_fpscam.yaw)*40,
                           g_fpscam.pos.v[1], g_fpscam.pos.v[2] + fm_cosf(g_fpscam.yaw)*40 }};
        kiln_actor_spawn(PROFILE_GRUNT, p2, g_fpscam.yaw, NULL);
    }
}

static const KilnActorProfile PLAYER_PROFILE = {
    .name = "player", .category = KILN_ACTOR_CAT_PLAYER, .state_size = 0,
    .event = player_event,
};

// ── Main ───────────────────────────────────────────────────────────────
int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    dfs_init(DFS_DEFAULT_LOCATION);
    kiln_input_init();
    kiln_audio_init(KILN_AUDIO_DEFAULT);

    int sfx_imp = kiln_sfx_load("rom:/sfx/impact.wav64");
    int sfx_met = kiln_sfx_load("rom:/sfx/impact_metal.wav64");
    kiln_surface_register(0, &(KilnSurfaceDef){.friction=0.9f,.footstep_sfx=sfx_imp});
    kiln_surface_register(1, &(KilnSurfaceDef){.friction=1.0f,.footstep_sfx=sfx_met});

    KilnSoundShader shaders[] = {
        {"gunshot","rom:/sfx/gunshot.wav64",0.8f,0},
        {"impact","rom:/sfx/impact.wav64",0.5f,0},
        {"enemy_hit","rom:/sfx/enemy_hit.wav64",0.6f,200},
        {"pickup","rom:/sfx/pickup.wav64",0.6f,150},
        {"door_open","rom:/sfx/door_open.wav64",0.5f,150},
        {"door_locked","rom:/sfx/door_locked.wav64",0.4f,150},
        {"chest_open","rom:/sfx/chest_open.wav64",0.5f,150},
        {"explosion","rom:/sfx/explosion.wav64",0.8f,300},
        {"rocket_fire","rom:/sfx/rocket_fire.wav64",0.6f,0},
        {"plasma_fire","rom:/sfx/plasma_fire.wav64",0.4f,0},
        {"shotgun_fire","rom:/sfx/shotgun_fire.wav64",0.7f,0},
        {"npc_talk","rom:/sfx/npc_talk.wav64",0.3f,100},
    };
    kiln_sound_init(shaders, 12);

    kiln_actor_system_init(PROFILES, PROFILE_COUNT, g_pool, ACTOR_POOL_CAP);
    /* Register the player profile separately (not in PROFILES array). */
    kiln_actor_spawn(0xFFFF, (fm_vec3_t){{0,0,0}}, 0, NULL); /* dummy, replaced below */
    kiln_event_init();
    kiln_projectile_init();
    kiln_projectile_set_hit_fn(proj_hit_cb);
    kiln_trigger_init();
    kiln_inventory_init(&g_inv);
    memset(&g_dialogue, 0, sizeof(g_dialogue));

    kiln_map_register_classname("info_enemy", PROFILE_GRUNT);
    kiln_map_register_classname("info_heavy", PROFILE_HEAVY);
    kiln_map_register_classname("info_health", PROFILE_HEALTH_PICKUP);
    kiln_map_register_classname("info_ammo", PROFILE_AMMO_PICKUP);
    kiln_map_register_classname("info_armor", PROFILE_ARMOR_PICKUP);
    kiln_map_register_classname("info_key_red", PROFILE_KEY_PICKUP);
    kiln_map_register_classname("info_npc", PROFILE_NPC);
    kiln_map_register_classname("info_chest", PROFILE_CHEST);
    kiln_map_register_classname("info_key_door", PROFILE_KEY_DOOR);
    kiln_map_register_classname("info_switch", PROFILE_SWITCH);
    kiln_map_register_classname("info_barrel", PROFILE_BARREL);
    kiln_map_register_classname("info_player_start", PROFILE_GRUNT);

    memset(g_rooms, 0, sizeof(g_rooms));
    memset(g_room_loaded, 0, sizeof(g_room_loaded));
    g_rooms[0].aabb_min = (fm_vec3_t){{-160,0,-160}}; g_rooms[0].aabb_max = (fm_vec3_t){{160,48,0}};
    g_rooms[0].neighbours[0]=1; g_rooms[0].neighbour_count=1;
    g_rooms[1].aabb_min = (fm_vec3_t){{-40,0,0}}; g_rooms[1].aabb_max = (fm_vec3_t){{40,48,160}};
    g_rooms[1].neighbours[0]=0; g_rooms[1].neighbours[1]=2; g_rooms[1].neighbour_count=2;
    g_rooms[2].aabb_min = (fm_vec3_t){{-160,0,0}}; g_rooms[2].aabb_max = (fm_vec3_t){{160,48,160}};
    g_rooms[2].neighbours[0]=1; g_rooms[2].neighbour_count=1;

    for (int i = 0; i < ROOM_COUNT; i++) {
        char path[64]; snprintf(path, sizeof(path), "rom:/maps/fps_room%d.map", i);
        KilnMap tmp;
        if (kiln_map_load(&tmp, path) >= 0) {
            for (int j = 0; j < tmp.spawn_count && j < KILN_ROOM_MAX_SPAWNS; j++)
                g_rooms[i].spawns[j] = tmp.spawns[j];
            g_rooms[i].spawn_count = tmp.spawn_count;
            for (int j = 0; j < tmp.spawn_count; j++) {
                if (tmp.spawns[j].pos.v[2] < -50) { g_spawn_pos = tmp.spawns[j].pos; g_spawn_yaw = tmp.spawns[j].yaw; }
            }
            kiln_map_free(&tmp);
        }
    }

    kiln_room_system_init(&g_room_sys, g_rooms, ROOM_COUNT, 4,
        on_room_load, on_room_unload, on_room_spawn, on_room_draw, NULL, 1);

    kiln_fpscam_init(&g_fpscam);
    kiln_fpscam_snap(&g_fpscam, g_spawn_pos, g_spawn_yaw, 0);
    kiln_weapons_init(&g_wset, WEAPON_DEFS, 4);
    kiln_scene_init(&g_scene);
    g_scene.far_z = 600; g_scene.ambient[3] = 255; g_scene.clear_color = RGBA32(8,8,16,255);

    g_cube_grunt=make_color_cube(10,0xC81E1EFF); g_cube_heavy=make_color_cube(14,0x8B0000FF);
    g_cube_health=make_color_cube(6,0x00C853FF); g_cube_ammo=make_color_cube(6,0xFFD600FF);
    g_cube_armor=make_color_cube(6,0x4488FFFF); g_cube_key=make_color_cube(6,0xFF4444FF);
    g_cube_npc=make_color_cube(10,0x88CCFFFF); g_cube_chest_c=make_color_cube(12,0x8B4513FF);
    g_cube_chest_o=make_color_cube(12,0x00C853FF); g_cube_door_c=make_color_cube(14,0x8B0000FF);
    g_cube_door_o=make_color_cube(14,0x00FF00FF); g_cube_switch=make_color_cube(6,0x00F5D4FF);
    g_cube_barrel=make_color_cube(8,0xFF8800FF);

    /* Spawn the player actor (for receiving trigger events). */
    {
        KilnActorProfile pp = PLAYER_PROFILE;
        /* Directly init pool with extra profile at index PROFILE_COUNT.
         * Simpler: just spawn at a known slot. Use a dummy approach:
         * re-init the pool with an extended profile table. */
        static KilnActorProfile ext_profiles[PROFILE_COUNT+1];
        memcpy(ext_profiles, PROFILES, sizeof(PROFILES));
        ext_profiles[PROFILE_COUNT] = PLAYER_PROFILE;
        kiln_actor_system_init(ext_profiles, PROFILE_COUNT+1, g_pool, ACTOR_POOL_CAP);
        KilnActorHandle ph = kiln_actor_spawn(PROFILE_COUNT, g_spawn_pos, g_spawn_yaw, NULL);
        g_player_actor = kiln_actor_resolve(ph);
    }

    uint32_t frames = 0; float fps = 0; uint32_t last_ticks = get_ticks(); float ta = 0;

    for (;;) {
        kiln_input_update();
        const KilnInput *in = kiln_input_get(1);
        float dt = 1.0f/60.0f; ta += dt;

        kiln_room_system_update(&g_room_sys, g_fpscam.pos);

        int dialogue_active = kiln_dialogue_active(&g_dialogue);

        if (!dialogue_active) {
            kiln_fpscam_update(&g_fpscam, in, dt);
            kiln_weapons_update(&g_wset, dt);
        }

        /* Z-targeting (OoT). */
        if (!dialogue_active && (in->buttons & KILN_BTN_Z)) {
            if (g_z_target == KILN_ACTOR_HANDLE_NONE) {
                fm_vec3_t fwd = kiln_fpscam_forward(&g_fpscam);
                g_z_target = kiln_target_acquire(g_fpscam.pos, fwd, 0.5f, 300);
            }
        } else {
            g_z_target = KILN_ACTOR_HANDLE_NONE;
        }

        /* Weapon switching (Doom): D-pad L/R. */
        if (!dialogue_active) {
            if (in->edges & KILN_BTN_DL) kiln_weapons_prev(&g_wset);
            if (in->edges & KILN_BTN_DR) kiln_weapons_next(&g_wset);
            if (in->edges & KILN_BTN_R) kiln_weapons_reload(&g_wset);
        }

        /* Fire. */
        if (!dialogue_active && (in->buttons & KILN_BTN_A) && g_ctx_action == KILN_CTX_NONE && g_z_target == KILN_ACTOR_HANDLE_NONE)
            fire_weapon();

        /* Context-action (OoT A-button). */
        if (!dialogue_active) {
            g_ctx_action = kiln_context_scan(g_fpscam.pos, g_fpscam.yaw, 30, 0.5f, &g_ctx_actor);
            if (g_ctx_action != KILN_CTX_NONE && (in->edges & KILN_BTN_A)) {
                switch (g_ctx_action) {
                case KILN_CTX_TALK: {
                    KilnActor *a = kiln_actor_resolve(g_ctx_actor);
                    if (a) {
                        NpcState *n = (void *)a->state;
                        const char *lines[] = { n->line };
                        kiln_dialogue_start(&g_dialogue, lines, 1);
                        kiln_sound_play("npc_talk", a->xform.pos, 1);
                    }
                    break;
                }
                case KILN_CTX_OPEN: {
                    KilnActor *a = kiln_actor_resolve(g_ctx_actor);
                    if (a && a->profile_id == PROFILE_KEY_DOOR) {
                        DoorState *d = (void *)a->state;
                        if (d->open) break;
                        if (kiln_inventory_has(&g_inv, d->key_id)) {
                            kiln_inventory_consume(&g_inv, d->key_id, 1);
                            int32_t args[1] = {1};
                            kiln_event_post(g_ctx_actor, EV_DOOR_OPEN, 0, args, 1, 1);
                        } else {
                            kiln_sound_play("door_locked", a->xform.pos, 1);
                        }
                    }
                    break;
                }
                case KILN_CTX_UNLOCK: {
                    KilnActor *a = kiln_actor_resolve(g_ctx_actor);
                    if (a) {
                        DoorState *d = (void *)a->state;
                        if (kiln_inventory_has(&g_inv, d->key_id)) {
                            kiln_inventory_consume(&g_inv, d->key_id, 1);
                            int32_t args[1] = {1};
                            kiln_event_post(g_ctx_actor, EV_DOOR_OPEN, 0, args, 1, 1);
                        } else {
                            kiln_sound_play("door_locked", a->xform.pos, 1);
                        }
                    }
                    break;
                }
                case KILN_CTX_OPEN_CHEST: {
                    int32_t args[1] = {1};
                    kiln_event_post(g_ctx_actor, EV_CHEST_OPEN, 0, args, 1, 1);
                    break;
                }
                case KILN_CTX_USE: {
                    KilnActor *a = kiln_actor_resolve(g_ctx_actor);
                    if (a && a->profile_id == PROFILE_SWITCH) {
                        SwitchState *sw = (void *)a->state;
                        if (!sw->activated && sw->door != KILN_ACTOR_HANDLE_NONE) {
                            sw->activated = 1;
                            int32_t args[1] = {1};
                            kiln_event_post(sw->door, EV_DOOR_OPEN, 0, args, 1, 1);
                        }
                    }
                    break;
                }
                default: break;
                }
            }
        } else {
            g_ctx_action = KILN_CTX_NONE;
        }

        /* Jump: B button (only if no context action). */
        if (!dialogue_active && g_ctx_action == KILN_CTX_NONE && (in->edges & KILN_BTN_A) && g_z_target != KILN_ACTOR_HANDLE_NONE) {
            /* A is fire when targeting. B is jump. */
        }

        /* Trigger volumes (Half-Life). */
        fm_vec3_t push_vel;
        if (g_player_actor) {
            kiln_trigger_update(g_fpscam.pos, kiln_actor_handle_of(g_player_actor), dt, &push_vel);
            g_fpscam.pos.v[0] += push_vel.v[0] * dt;
            g_fpscam.pos.v[1] += push_vel.v[1] * dt;
            g_fpscam.pos.v[2] += push_vel.v[2] * dt;
        }

        kiln_projectile_update(dt);
        kiln_event_process(dt);
        kiln_actor_update_all(dt);

        if (dialogue_active) kiln_dialogue_update(&g_dialogue, dt, in);

        if (g_player_health <= 0) {
            g_player_health = 100; g_player_armor = 0;
            kiln_fpscam_snap(&g_fpscam, g_spawn_pos, g_spawn_yaw, 0);
        }

        if (g_muzzle_flash>0) g_muzzle_flash-=dt;
        if (g_hitmarker>0) g_hitmarker-=dt;
        if (g_damage_flash>0) g_damage_flash-=dt;
        if (g_shake_t>0) g_shake_t-=dt;

        kiln_fpscam_apply(&g_fpscam, &g_scene);
        if (g_shake_t > 0) {
            float sh = g_shake_mag * (g_shake_t / 0.25f);
            g_scene.cam_pos.v[0] += fm_sinf(ta*40)*sh;
            g_scene.cam_pos.v[1] += fm_cosf(ta*37)*sh;
        }
        kiln_scene_update(&g_scene);

        fm_vec3_t fwd = kiln_fpscam_forward(&g_fpscam);
        kiln_sound_update_listener(g_fpscam.pos, fwd);

        if (++frames % 30 == 0) {
            uint32_t now = get_ticks();
            fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND);
            last_ticks = now;
        }

        /* ── 3D ───────────────────────────────────────────── */
        kiln_frame_begin();
        kiln_scene_begin(&g_scene);
        kiln_room_draw_all(&g_room_sys);
        kiln_actor_draw_all();
        kiln_projectile_draw_all();

        /* ── 2D ───────────────────────────────────────────── */
        kiln_gui_begin();

        if (g_damage_flash > 0) {
            uint8_t al = (uint8_t)(150 * (g_damage_flash / 0.3f));
            kiln_gui_rect(0, 0, SCREEN_W, SCREEN_H, RGBA32(200, 0, 0, al));
        }

        /* Crosshair. */
        { int cx=SCREEN_W/2, cy=SCREEN_H/2; color_t c=RGBA32(0,245,212,255);
        kiln_gui_rect(cx-1,cy-6,2,5,c); kiln_gui_rect(cx-1,cy+2,2,5,c);
        kiln_gui_rect(cx-6,cy-1,5,2,c); kiln_gui_rect(cx+2,cy-1,5,2,c);
        if (g_hitmarker>0) { color_t h=RGBA32(255,255,255,255);
            kiln_gui_rect(cx-8,cy-8,4,1,h); kiln_gui_rect(cx+5,cy-8,4,1,h);
            kiln_gui_rect(cx-8,cy+7,4,1,h); kiln_gui_rect(cx+5,cy+7,4,1,h); }
        if (g_muzzle_flash>0) { color_t m=RGBA32(255,220,80,200);
            kiln_gui_rect(cx-3,cy-1,7,2,m); kiln_gui_rect(cx-1,cy-3,2,7,m); }
        }

        /* Z-target reticle. */
        if (g_z_target != KILN_ACTOR_HANDLE_NONE) {
            KilnActor *ta = kiln_actor_resolve(g_z_target);
            if (ta) kiln_target_draw_reticle(&g_scene, ta->xform.pos, SCREEN_W, SCREEN_H, RGBA32(255,200,0,255));
        }

        /* Context prompt (OoT). */
        if (g_ctx_action != KILN_CTX_NONE) {
            const char *lbl = kiln_context_label(g_ctx_action);
            kiln_gui_text(SCREEN_W/2 - 30, SCREEN_H/2 + 20, RGBA32(255,255,0,255), "A: %s", lbl);
        }

        /* HUD: top-left. */
        kiln_gui_panel(8, 8, 180, 76, RGBA32(10,10,24,200), RGBA32(0,245,212,255));
        const KilnWeaponDef *wd = kiln_weapons_active_def(&g_wset);
        KilnWeapon *ws = kiln_weapons_active_state(&g_wset);
        kiln_gui_text(14, 20, RGBA32(0,245,212,255), "KILN FPS");
        kiln_gui_text(14, 32, RGBA32(232,232,240,255), "fps %5.1f", fps);
        kiln_gui_text(14, 44, RGBA32(232,232,240,255), "score %d", g_score);
        kiln_gui_text(14, 56, RGBA32(232,232,240,255), "enemies %d", kiln_actor_count(KILN_ACTOR_CAT_ENEMY));
        kiln_gui_text(14, 68, RGBA32(232,232,240,255), "wpn: %s", wd ? wd->name : "?");

        /* HUD: top-right health + armor. */
        int hb_w=100, hb_x=SCREEN_W-hb_w-8;
        kiln_gui_text(hb_x, 10, RGBA32(232,232,240,255), "HP");
        kiln_gui_bar(hb_x, 22, hb_w, 10, (float)g_player_health/100, RGBA32(200,30,30,255), RGBA32(40,40,40,255));
        kiln_gui_text(hb_x, 38, RGBA32(232,232,240,255), "ARM");
        kiln_gui_bar(hb_x, 50, hb_w, 10, (float)g_player_armor/100, RGBA32(68,136,255,255), RGBA32(40,40,40,255));

        /* HUD: bottom-left ammo. */
        if (ws) {
            const char *st = ws->state == KILN_WPN_RELOADING ? "RELOAD" : ws->state == KILN_WPN_FIRING ? "FIRE" : "IDLE";
            kiln_gui_panel(8, SCREEN_H-48, 140, 40, RGBA32(10,10,24,200), RGBA32(0,245,212,255));
            kiln_gui_text(14, SCREEN_H-38, RGBA32(232,232,240,255), "AMMO %d/%d [%s]", ws->magazine, ws->ammo, st);
            if (ws->state == KILN_WPN_RELOADING) {
                float prog = 1.0f - (ws->timer / ws->reload_time);
                kiln_gui_bar(14, SCREEN_H-22, 128, 6, prog, RGBA32(0,245,212,255), RGBA32(40,40,40,255));
            }
        }

        /* HUD: inventory keys. */
        if (kiln_inventory_has(&g_inv, ITEM_KEY_RED)) kiln_gui_text(SCREEN_W-40, 66, RGBA32(255,80,80,255), "[RED]");

        /* Dialogue box. */
        if (dialogue_active) kiln_dialogue_draw(&g_dialogue);

        /* Controls. */
        kiln_gui_text(8, SCREEN_H-10, RGBA32(139,92,246,255),
            "stick:move C:look A:fire/act Z:target B:jump R:reload DL/DR:wep");

        kiln_gui_end();
        kiln_frame_end();

        kiln_sound_update();
        kiln_audio_update();
    }
}