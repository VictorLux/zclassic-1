/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for P2P game engine. */

#include "test/test_helpers.h"
#include "net/p2p_game.h"

int test_game(void)
{
    int failures = 0;

    printf("ttt: init state... ");
    {
        struct ttt_state s;
        ttt_init(&s);
        bool ok = (s.turn == 1 && s.winner == 0 && s.move_count == 0);
        for (int i = 0; i < 9; i++) ok = ok && (s.board[i] == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("ttt: valid move... ");
    {
        struct ttt_state s;
        ttt_init(&s);
        bool ok = ttt_move(&s, 4, 1); /* X plays center */
        ok = ok && (s.board[4] == 1) && (s.turn == 2) && (s.move_count == 1);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("ttt: reject invalid position... ");
    {
        struct ttt_state s;
        ttt_init(&s);
        bool ok = !ttt_move(&s, 9, 1); /* out of bounds */
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("ttt: reject wrong turn... ");
    {
        struct ttt_state s;
        ttt_init(&s);
        bool ok = !ttt_move(&s, 0, 2); /* O can't go first */
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("ttt: reject occupied cell... ");
    {
        struct ttt_state s;
        ttt_init(&s);
        ttt_move(&s, 4, 1);
        bool ok = !ttt_move(&s, 4, 2); /* cell taken */
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("ttt: X wins row... ");
    {
        struct ttt_state s;
        ttt_init(&s);
        ttt_move(&s, 0, 1); /* X */
        ttt_move(&s, 3, 2); /* O */
        ttt_move(&s, 1, 1); /* X */
        ttt_move(&s, 4, 2); /* O */
        ttt_move(&s, 2, 1); /* X wins top row */
        bool ok = (s.winner == 1);
        if (ok) printf("OK\n");
        else { printf("FAIL (winner=%d)\n", s.winner); failures++; }
    }

    printf("ttt: O wins diagonal... ");
    {
        struct ttt_state s;
        ttt_init(&s);
        ttt_move(&s, 0, 1); /* X */
        ttt_move(&s, 2, 2); /* O */
        ttt_move(&s, 1, 1); /* X */
        ttt_move(&s, 4, 2); /* O */
        ttt_move(&s, 5, 1); /* X */
        ttt_move(&s, 6, 2); /* O wins 2-4-6 diagonal */
        bool ok = (s.winner == 2);
        if (ok) printf("OK\n");
        else { printf("FAIL (winner=%d)\n", s.winner); failures++; }
    }

    printf("ttt: draw... ");
    {
        struct ttt_state s;
        ttt_init(&s);
        ttt_move(&s, 0, 1); ttt_move(&s, 1, 2);
        ttt_move(&s, 2, 1); ttt_move(&s, 4, 2);
        ttt_move(&s, 3, 1); ttt_move(&s, 5, 2);
        ttt_move(&s, 7, 1); ttt_move(&s, 6, 2);
        ttt_move(&s, 8, 1);
        bool ok = (s.winner == 3 && s.move_count == 9);
        if (ok) printf("OK\n");
        else { printf("FAIL (winner=%d moves=%d)\n", s.winner, s.move_count); failures++; }
    }

    printf("ttt: reject move after win... ");
    {
        struct ttt_state s;
        ttt_init(&s);
        ttt_move(&s, 0, 1); ttt_move(&s, 3, 2);
        ttt_move(&s, 1, 1); ttt_move(&s, 4, 2);
        ttt_move(&s, 2, 1); /* X wins */
        bool ok = !ttt_move(&s, 5, 2); /* can't move after win */
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("ttt: render board... ");
    {
        struct ttt_state s;
        ttt_init(&s);
        ttt_move(&s, 4, 1); ttt_move(&s, 0, 2);
        char buf[256];
        ttt_render(&s, buf, sizeof(buf));
        bool ok = (strstr(buf, "X") != NULL && strstr(buf, "O") != NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("game: serialize invite... ");
    {
        uint8_t buf[32];
        size_t n = game_serialize_invite(buf, sizeof(buf), GAME_TICTACTOE);
        bool ok = (n == 2 && buf[0] == GAME_TICTACTOE && buf[1] == GAME_INVITE);
        if (ok) printf("OK (%zu bytes)\n", n);
        else { printf("FAIL\n"); failures++; }
    }

    printf("game: serialize move... ");
    {
        uint8_t buf[32];
        size_t n = game_serialize_move(buf, sizeof(buf), 4);
        bool ok = (n == 11 && buf[1] == GAME_MOVE && buf[2] == 4);
        if (ok) printf("OK (%zu bytes)\n", n);
        else { printf("FAIL (n=%zu)\n", n); failures++; }
    }

    printf("game: serialize/deserialize state roundtrip... ");
    {
        struct ttt_state s;
        ttt_init(&s);
        ttt_move(&s, 4, 1); ttt_move(&s, 0, 2); ttt_move(&s, 8, 1);

        uint8_t buf[32];
        size_t n = game_serialize_state(buf, sizeof(buf), &s);

        struct ttt_state s2;
        memset(&s2, 0, sizeof(s2));
        uint8_t gt;
        game_deserialize(buf, n, &gt, NULL, &s2);

        bool ok = (gt == GAME_TICTACTOE &&
                   memcmp(s.board, s2.board, 9) == 0 &&
                   s2.turn == s.turn &&
                   s2.winner == s.winner &&
                   s2.move_count == s.move_count);
        if (ok) printf("OK (%zu bytes)\n", n);
        else { printf("FAIL\n"); failures++; }
    }

    printf("game: deserialize move extracts position... ");
    {
        uint8_t buf[32];
        game_serialize_move(buf, sizeof(buf), 7);
        uint8_t pos = 255;
        uint8_t gt;
        enum game_action a = game_deserialize(buf, 11, &gt, &pos, NULL);
        bool ok = (a == GAME_MOVE && pos == 7 && gt == GAME_TICTACTOE);
        if (ok) printf("OK (pos=%d)\n", pos);
        else { printf("FAIL\n"); failures++; }
    }

    printf("game: latency timestamp in move... ");
    {
        uint8_t buf[32];
        size_t n = game_serialize_move(buf, sizeof(buf), 0);
        /* Timestamp is at bytes 3-10 (8 bytes, int64_t) */
        int64_t ts = 0;
        if (n >= 11) memcpy(&ts, buf + 3, 8);
        bool ok = (ts > 1700000000LL * 1000000); /* after 2023 in microseconds */
        if (ok) printf("OK (ts=%lld us)\n", (long long)ts);
        else { printf("FAIL (ts=%lld)\n", (long long)ts); failures++; }
    }

    return failures;
}
