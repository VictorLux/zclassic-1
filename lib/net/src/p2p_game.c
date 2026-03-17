/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * P2P game engine — tic-tac-toe for latency testing. */

#include "net/p2p_game.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

static int64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/* ── Tic-tac-toe logic ───────────────────────────────────── */

void ttt_init(struct ttt_state *s)
{
    memset(s, 0, sizeof(*s));
    s->turn = 1; /* X goes first */
    s->game_start_us = now_us();
}

bool ttt_move(struct ttt_state *s, uint8_t pos, uint8_t player)
{
    if (pos >= 9) return false;
    if (s->board[pos] != 0) return false;
    if (s->turn != player) return false;
    if (s->winner != 0) return false;

    s->board[pos] = player;
    s->turn = (player == 1) ? 2 : 1;
    s->move_count++;
    s->last_move_us = now_us();

    ttt_check_winner(s);
    return true;
}

void ttt_check_winner(struct ttt_state *s)
{
    /* Check rows, columns, diagonals */
    static const uint8_t wins[8][3] = {
        {0,1,2}, {3,4,5}, {6,7,8}, /* rows */
        {0,3,6}, {1,4,7}, {2,5,8}, /* cols */
        {0,4,8}, {2,4,6}           /* diags */
    };

    for (int i = 0; i < 8; i++) {
        uint8_t a = s->board[wins[i][0]];
        uint8_t b = s->board[wins[i][1]];
        uint8_t c = s->board[wins[i][2]];
        if (a != 0 && a == b && b == c) {
            s->winner = a;
            return;
        }
    }

    /* Check draw */
    bool full = true;
    for (int i = 0; i < 9; i++)
        if (s->board[i] == 0) { full = false; break; }
    if (full) s->winner = 3; /* draw */
}

void ttt_render(const struct ttt_state *s, char *out, size_t max)
{
    static const char sym[] = ".XO";
    snprintf(out, max,
        " %c | %c | %c \n"
        "---+---+---\n"
        " %c | %c | %c \n"
        "---+---+---\n"
        " %c | %c | %c \n"
        "Turn: %c  Moves: %u  %s",
        sym[s->board[0]], sym[s->board[1]], sym[s->board[2]],
        sym[s->board[3]], sym[s->board[4]], sym[s->board[5]],
        sym[s->board[6]], sym[s->board[7]], sym[s->board[8]],
        sym[s->turn], s->move_count,
        s->winner == 1 ? "X wins!" :
        s->winner == 2 ? "O wins!" :
        s->winner == 3 ? "Draw!" : "");
}

/* ── Wire format serialization ───────────────────────────── */

size_t game_serialize_invite(uint8_t *out, size_t max, enum game_type type)
{
    if (max < 2) return 0;
    out[0] = (uint8_t)type;
    out[1] = GAME_INVITE;
    return 2;
}

size_t game_serialize_accept(uint8_t *out, size_t max, uint8_t side)
{
    if (max < 3) return 0;
    out[0] = GAME_TICTACTOE;
    out[1] = GAME_ACCEPT;
    out[2] = side;
    return 3;
}

size_t game_serialize_move(uint8_t *out, size_t max, uint8_t position)
{
    if (max < 4) return 0;
    out[0] = GAME_TICTACTOE;
    out[1] = GAME_MOVE;
    out[2] = position;
    /* Timestamp for latency measurement */
    int64_t ts = now_us();
    if (max >= 11) {
        memcpy(out + 3, &ts, 8);
        return 11;
    }
    return 3;
}

size_t game_serialize_state(uint8_t *out, size_t max,
                             const struct ttt_state *state)
{
    if (max < 14) return 0;
    out[0] = GAME_TICTACTOE;
    out[1] = GAME_STATE;
    memcpy(out + 2, state->board, 9);
    out[11] = state->turn;
    out[12] = state->winner;
    out[13] = (uint8_t)state->move_count;
    return 14;
}

enum game_action game_deserialize(const uint8_t *data, size_t len,
                                   uint8_t *game_type_out,
                                   uint8_t *position_out,
                                   struct ttt_state *state_out)
{
    if (!data || len < 2) return GAME_RESIGN;
    if (game_type_out) *game_type_out = data[0];

    enum game_action action = (enum game_action)data[1];

    switch (action) {
    case GAME_INVITE:
        return GAME_INVITE;

    case GAME_ACCEPT:
        return GAME_ACCEPT;

    case GAME_MOVE:
        if (len >= 3 && position_out) *position_out = data[2];
        return GAME_MOVE;

    case GAME_STATE:
        if (len >= 14 && state_out) {
            memcpy(state_out->board, data + 2, 9);
            state_out->turn = data[11];
            state_out->winner = data[12];
            state_out->move_count = data[13];
        }
        return GAME_STATE;

    default:
        return action;
    }
}
