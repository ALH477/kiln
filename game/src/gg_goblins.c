// SPDX-License-Identifier: MPL-2.0

#include "gg_goblins.h"
#include "gg_specials.h"

const M64CharProfile gg_goblins[GG_GOBLIN_COUNT] = {
    [GG_GOBLIN_DANK] = {
        .id               = GG_GOBLIN_DANK,
        .name             = "Dank",
        .passive_desc     = "+1 bud per Grow space",
        .special_desc     = "Couch Lock: freeze a rival for a turn",
        .charge_threshold = 10,
        .passive          = gg_passive_dank,
        .special          = gg_special_dank,
    },
    [GG_GOBLIN_SPARKY] = {
        .id               = GG_GOBLIN_SPARKY,
        .name             = "Sparky",
        .passive_desc     = "+1 movement every 3rd turn",
        .special_desc     = "Spark Plug: force a mini-game with bonus rewards",
        .charge_threshold = 12,
        .passive          = gg_passive_sparky,
        .special          = gg_special_sparky,
    },
    [GG_GOBLIN_MOSS] = {
        .id               = GG_GOBLIN_MOSS,
        .name             = "Moss",
        .passive_desc     = "items last +1 turn",
        .special_desc     = "Resin Trap: leave a sticky hazard that slows rivals",
        .charge_threshold = 10,
        .passive          = gg_passive_moss,
        .special          = gg_special_moss,
    },
    [GG_GOBLIN_GLIMMER] = {
        .id               = GG_GOBLIN_GLIMMER,
        .name             = "Glimmer",
        .passive_desc     = "+1 to dice when behind",
        .special_desc     = "Lucky Puff: steal a random bud from every other player",
        .charge_threshold = 8,
        .passive          = gg_passive_glimmer,
        .special          = gg_special_glimmer,
    },
};