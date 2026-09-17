/* prefix_equiv.h — two spellings of the same thing, so a cached prefix survives.
 *
 * THE PROBLEM. A chat client resends the whole transcript every turn, and the
 * engine reuses its attention state only if the new prompt begins with exactly
 * the token ids that state was built from. On glm53 that test is
 * all-or-nothing: the KDA layers hold a running total no one can unfold, so a
 * single differing id anywhere in the cached span throws all of it away.
 *
 * That is fine while the client replays what the model wrote. It is not fine
 * when a chat template and a model disagree about whitespace. GLM-5.3 writes
 * `</think><tool_call>` on a turn that is nothing but a tool call; the official
 * template writes `</think>` NEWLINE `<tool_call>`. One token, and the next
 * turn re-prefills from scratch — twenty minutes on a 3k-token agent history
 * at 2.3 tok/s (#1576).
 *
 * THE IDEA. Keep a small table of forms that mean the same thing, in TOKEN IDS
 * rather than text, and when the walk hits a difference, ask the table whether
 * this particular difference is one of them. If it is, rewrite the new prompt
 * into the spelling the cache already holds and keep walking.
 *
 * WHY REWRITE THE PROMPT AND NOT MERELY SKIP. The two forms have different
 * lengths. The cached state's positions were numbered without the newline, and
 * every position after it in the tape sits at that numbering. Agreeing to
 * disagree would leave the rest of the prompt offset by one against the state
 * it is being spliced onto — the failure that does not crash, it just answers
 * from the wrong context. So the cache is taken as the truth (it is also what
 * the model actually produced, which the template is not) and the prompt is
 * spliced to match.
 *
 * WHY IDS AND NOT STRINGS. The same text tokenizes differently per model, and
 * a string search would also have to worry about where a token boundary falls.
 * Callers encode both spellings once with their own tokenizer at startup and
 * hand the ids over; this file never sees text.
 *
 * SCOPE. This forgives only what it is told to forgive. It is not a fuzzy
 * match: an unlisted difference ends the walk exactly as before, so the worst
 * case is today's behaviour. A pair is safe to add when the two forms are the
 * same input written two ways -- never when they carry different meaning.
 */
#ifndef PREFIX_EQUIV_H
#define PREFIX_EQUIV_H

#include <string.h>

#define COLI_EQUIV_MAX_PAIRS 8
#define COLI_EQUIV_MAX_LEN   8

typedef struct {
    int cached[COLI_EQUIV_MAX_LEN];   /* the spelling the cache holds */
    int n_cached;
    int prompt[COLI_EQUIV_MAX_LEN];   /* the spelling the client sent */
    int n_prompt;
} ColiEquivPair;

typedef struct {
    ColiEquivPair pair[COLI_EQUIV_MAX_PAIRS];
    int n;
} ColiEquivTable;

/* Register one equivalence. Returns 1 when stored, 0 when refused.
 *
 * Refusals are deliberate and silent-safe: a full table, an empty or oversized
 * form, or two sides that are identical (which would "forgive" a position that
 * cannot differ, and make no progress). Sizes are checked before either side is
 * read, so a bad length cannot walk off the caller's array. */
static inline int coli_equiv_add(ColiEquivTable *t,
                                 const int *cached, int n_cached,
                                 const int *prompt, int n_prompt) {
    if (!t || !cached || !prompt) return 0;
    if (t->n >= COLI_EQUIV_MAX_PAIRS) return 0;
    if (n_cached < 1 || n_prompt < 1) return 0;
    if (n_cached > COLI_EQUIV_MAX_LEN || n_prompt > COLI_EQUIV_MAX_LEN) return 0;
    if (n_cached == n_prompt &&
        !memcmp(cached, prompt, (size_t)n_cached * sizeof(int))) return 0;
    ColiEquivPair *p = &t->pair[t->n];
    memcpy(p->cached, cached, (size_t)n_cached * sizeof(int));
    memcpy(p->prompt, prompt, (size_t)n_prompt * sizeof(int));
    p->n_cached = n_cached;
    p->n_prompt = n_prompt;
    t->n++;
    return 1;
}

/* Length of the prefix `cached` and `prompt` share, with listed differences
 * forgiven and `prompt` rewritten in place into the cache's spelling.
 *
 * `n_prompt` is updated because a rewrite changes the length; `cap` is the
 * prompt buffer's capacity, and a rewrite that would not fit is declined rather
 * than truncated. A NULL or empty table makes this a plain comparison.
 *
 * The anchor is searched backwards from the difference, because the forms
 * usually agree on their first token (`</think>` in the case above) and the walk
 * has therefore already stepped past where the form began. A candidate must
 * extend past the difference on the cached side, which is also what guarantees
 * the walk advances and cannot spin. */
static inline int coli_equiv_reconcile(const ColiEquivTable *t,
                                       const int *cached, int n_cached,
                                       int *prompt, int *n_prompt, int cap) {
    int i = 0;
    for (;;) {
        while (i < n_cached && i < *n_prompt && cached[i] == prompt[i]) i++;
        if (i >= n_cached || i >= *n_prompt) return i;
        if (!t || t->n <= 0) return i;

        int matched = 0;
        for (int k = 0; k < t->n && !matched; k++) {
            const ColiEquivPair *p = &t->pair[k];
            int lo = i - COLI_EQUIV_MAX_LEN + 1;
            if (lo < 0) lo = 0;
            for (int j = i; j >= lo && !matched; j--) {
                if (j + p->n_cached > n_cached) continue;   /* cache half-holds it */
                if (j + p->n_prompt > *n_prompt) continue;  /* prompt half-holds it */
                if (j + p->n_cached <= i) continue;         /* would not advance */
                if (memcmp(cached + j, p->cached,
                           (size_t)p->n_cached * sizeof(int))) continue;
                if (memcmp(prompt + j, p->prompt,
                           (size_t)p->n_prompt * sizeof(int))) continue;
                int grown = *n_prompt + p->n_cached - p->n_prompt;
                if (grown > cap) continue;                  /* decline, never truncate */
                memmove(prompt + j + p->n_cached, prompt + j + p->n_prompt,
                        (size_t)(*n_prompt - j - p->n_prompt) * sizeof(int));
                memcpy(prompt + j, p->cached, (size_t)p->n_cached * sizeof(int));
                *n_prompt = grown;
                i = j + p->n_cached;
                matched = 1;
            }
        }
        if (!matched) return i;
    }
}

#endif
