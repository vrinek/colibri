/* test_prefix_equiv — the "these two spellings are the same tokens" table.
 *
 * This decides whether a cached prefix survives, so a wrong answer here is the
 * expensive kind: too strict and every tool-calling turn re-prefills (#1576),
 * too loose and the engine answers from a state built out of different text.
 * Every rejection rule gets a case, including the paranoid ones, and so does
 * the rewrite arithmetic — the sequence is spliced in place, so an off-by-one
 * there corrupts the prompt rather than merely slowing it down.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../prefix_equiv.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); failures++; } } while (0)

/* The real pair, with GLM-5.3's ids: the model writes `</think><tool_call>`,
 * the template writes `</think>` NEWLINE `<tool_call>`. */
#define THINK_END 154842
#define TOOL_CALL 154843
#define NEWLINE   198

static void seed(ColiEquivTable *t) {
    const int model[]    = {THINK_END, TOOL_CALL};
    const int template_[] = {THINK_END, NEWLINE, TOOL_CALL};
    CHECK(coli_equiv_add(t, model, 2, template_, 3), "seeding the real pair failed");
}

int main(void) {
    /* An empty table must behave exactly like a plain comparison: this is what
     * makes the feature safe to ship disabled. */
    {
        ColiEquivTable t = {0};
        int prompt[] = {1, 2, 3, 4};
        const int cached[] = {1, 2, 9, 4};
        int n = 4;
        CHECK(coli_equiv_reconcile(&t, cached, 4, prompt, &n, 4) == 2,
              "empty table must stop at the first difference");
        CHECK(n == 4, "empty table must not rewrite, n=%d", n);
        CHECK(prompt[2] == 3, "empty table must not touch the prompt");
    }

    /* Identical sequences: the answer is the shorter length, nothing rewritten. */
    {
        ColiEquivTable t = {0}; seed(&t);
        int prompt[] = {7, 8, 9};
        const int cached[] = {7, 8, 9, 10, 11};
        int n = 3;
        CHECK(coli_equiv_reconcile(&t, cached, 5, prompt, &n, 3) == 3,
              "a prompt that is a prefix of the cache shares all of itself");
        CHECK(n == 3, "no rewrite expected, n=%d", n);
    }

    /* The case from #1576. The cache holds what the model generated; the prompt
     * arrives in template spelling. The whole cached span must survive, and the
     * prompt must come back in the cache's spelling so later positions line up. */
    {
        ColiEquivTable t = {0}; seed(&t);
        const int cached[] = {100, THINK_END, TOOL_CALL, 200};
        int prompt[8] = {100, THINK_END, NEWLINE, TOOL_CALL, 200, 300};
        int n = 6;
        int common = coli_equiv_reconcile(&t, cached, 4, prompt, &n, 8);
        CHECK(common == 4, "want the whole cached span shared, got %d", common);
        CHECK(n == 5, "the newline must be spliced out, n=%d", n);
        const int want[] = {100, THINK_END, TOOL_CALL, 200, 300};
        CHECK(!memcmp(prompt, want, sizeof want), "prompt not rewritten into the cache's form");
    }

    /* Forgiveness has to repeat: a five-turn agent conversation carries one
     * bare tool call per turn, so a table that fires once is nearly useless. */
    {
        ColiEquivTable t = {0}; seed(&t);
        const int cached[] = {THINK_END, TOOL_CALL, 50, THINK_END, TOOL_CALL, 60};
        int prompt[16] = {THINK_END, NEWLINE, TOOL_CALL, 50, THINK_END, NEWLINE, TOOL_CALL, 60, 70};
        int n = 9;
        CHECK(coli_equiv_reconcile(&t, cached, 6, prompt, &n, 16) == 6,
              "two divergences in one prompt must both be forgiven");
        CHECK(n == 7, "both newlines must go, n=%d", n);
    }

    /* A difference that is NOT in the table still stops the walk, and the
     * position it reports is the real common prefix. */
    {
        ColiEquivTable t = {0}; seed(&t);
        const int cached[] = {1, THINK_END, TOOL_CALL, 4};
        int prompt[8] = {1, THINK_END, TOOL_CALL, 99};
        int n = 4;
        CHECK(coli_equiv_reconcile(&t, cached, 4, prompt, &n, 8) == 3,
              "an unlisted difference must not be forgiven");
        CHECK(n == 4, "a failed match must leave the prompt alone, n=%d", n);
    }

    /* A pair must not match on a partial occurrence at the end of either side:
     * the tail it needs has not arrived yet, so forgiving it would invent text. */
    {
        ColiEquivTable t = {0}; seed(&t);
        const int cached[] = {1, THINK_END};                 /* cache ends mid-form */
        int prompt[8] = {1, THINK_END, NEWLINE, TOOL_CALL};
        int n = 4;
        CHECK(coli_equiv_reconcile(&t, cached, 2, prompt, &n, 8) == 2,
              "a form the cache only half-contains must not match");
        CHECK(n == 4, "no rewrite on a partial match, n=%d", n);

        const int cached2[] = {1, THINK_END, TOOL_CALL, 4};
        int prompt2[8] = {1, THINK_END, NEWLINE};            /* prompt ends mid-form */
        int n2 = 3;
        CHECK(coli_equiv_reconcile(&t, cached2, 4, prompt2, &n2, 8) == 2,
              "a form the prompt only half-contains must not match");
        CHECK(n2 == 3, "no rewrite on a partial match, n2=%d", n2);
    }

    /* A pair whose cached form is LONGER than the prompt form grows the
     * sequence. That must respect the caller's buffer: refuse rather than
     * write past it. (No such pair ships today; the arithmetic still has to be
     * right, because the table is meant to be extended.) */
    {
        ColiEquivTable t = {0};
        const int long_form[] = {10, 11, 12};
        const int short_form[] = {10};
        CHECK(coli_equiv_add(&t, long_form, 3, short_form, 1), "add failed");

        int prompt[8] = {1, 10, 2};
        const int cached[] = {1, 10, 11, 12, 2};
        int n = 3;
        CHECK(coli_equiv_reconcile(&t, cached, 5, prompt, &n, 8) == 5,
              "a growing rewrite must still be forgiven when it fits");
        CHECK(n == 5, "the grown length must be reported, n=%d", n);
        const int want[] = {1, 10, 11, 12, 2};
        CHECK(!memcmp(prompt, want, sizeof want), "grown prompt is wrong");

        /* Same input, no room: the rewrite must be declined, not truncated. */
        int tight[3] = {1, 10, 2};
        int n_tight = 3;
        CHECK(coli_equiv_reconcile(&t, cached, 5, tight, &n_tight, 3) == 2,
              "a rewrite that would not fit must be declined");
        CHECK(n_tight == 3, "a declined rewrite must not change the length");
        CHECK(tight[1] == 10 && tight[2] == 2, "a declined rewrite must not write");
    }

    /* Defensive: a NULL table is inert, and the table refuses malformed pairs
     * rather than storing something it will later read out of bounds. */
    {
        int prompt[] = {1, 2};
        const int cached[] = {1, 9};
        int n = 2;
        CHECK(coli_equiv_reconcile(NULL, cached, 2, prompt, &n, 2) == 1,
              "a NULL table compares plainly");

        ColiEquivTable t = {0};
        const int ok[] = {1};
        CHECK(!coli_equiv_add(&t, ok, 0, ok, 1), "an empty side must be refused");
        CHECK(!coli_equiv_add(&t, ok, 1, ok, 0), "an empty side must be refused");
        CHECK(!coli_equiv_add(&t, ok, COLI_EQUIV_MAX_LEN + 1, ok, 1),
              "an oversized form must be refused");
        CHECK(t.n == 0, "nothing malformed may be stored, n=%d", t.n);

        /* Identical sides would loop forever at a position that cannot differ. */
        CHECK(!coli_equiv_add(&t, ok, 1, ok, 1), "identical sides must be refused");

        /* The table fills up and then politely refuses. */
        ColiEquivTable full = {0};
        const int a[] = {1, 2}, b[] = {1, 3};
        int added = 0;
        while (coli_equiv_add(&full, a, 2, b, 2)) added++;
        CHECK(added == COLI_EQUIV_MAX_PAIRS, "added %d pairs, cap is %d",
              added, COLI_EQUIV_MAX_PAIRS);
    }

    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    puts("prefix_equiv: ok");
    return 0;
}
