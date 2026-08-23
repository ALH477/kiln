// SPDX-License-Identifier: MIT

#include "kiln_board.h"

void kiln_board_init(KilnBoard *b, const KilnBoardNode *nodes, uint16_t node_count,
                    int16_t start_node)
{
    b->nodes = nodes;
    b->node_count = node_count;
    b->start_node = start_node;

    if (node_count == 0) {
        b->aabb_min = (fm_vec3_t){{ 0, 0, 0 }};
        b->aabb_max = (fm_vec3_t){{ 0, 0, 0 }};
        return;
    }

    fm_vec3_t lo = nodes[0].pos;
    fm_vec3_t hi = nodes[0].pos;
    for (uint16_t i = 1; i < node_count; i++) {
        for (int k = 0; k < 3; k++) {
            float v = nodes[i].pos.v[k];
            if (v < lo.v[k]) lo.v[k] = v;
            if (v > hi.v[k]) hi.v[k] = v;
        }
    }
    b->aabb_min = lo;
    b->aabb_max = hi;
}

int16_t kiln_board_step(const KilnBoard *b, int16_t cur, uint8_t branch)
{
    if (cur < 0 || cur >= (int16_t)b->node_count) return b->start_node;
    const KilnBoardNode *n = &b->nodes[cur];
    if (branch >= n->next_count) return cur;
    int16_t nxt = n->next[branch];
    if (nxt < 0 || nxt >= (int16_t)b->node_count) return cur;
    return nxt;
}

const fm_vec3_t *kiln_board_pos(const KilnBoard *b, int16_t idx)
{
    if (idx < 0 || idx >= (int16_t)b->node_count) idx = b->start_node;
    return &b->nodes[idx].pos;
}