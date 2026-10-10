/*
 * Host-native test of the buddy allocator core (kernel/mm/buddy.c) under
 * ASan/UBSan: exhaustive allocation, random alloc/free with an ownership map
 * to detect overlaps, alignment, full coalescing, reserved holes, and error
 * paths.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "buddy.h"

#define NPAGES   3000           /* deliberately not a power of two */
#define BASE_PFN 0x80000UL
#define HOLE_LO  1500           /* reserved pages [HOLE_LO, HOLE_HI) */
#define HOLE_HI  1537

static struct page pages[NPAGES];
static int owner[NPAGES];       /* -1 free, else allocation id */
static int failures, checks;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);          \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
        }                                                                   \
    } while (0)

static void setup(struct buddy_zone *z)
{
    buddy_init(z, pages, BASE_PFN, NPAGES);
    buddy_release_range(z, BASE_PFN, BASE_PFN + HOLE_LO);
    buddy_release_range(z, BASE_PFN + HOLE_HI, BASE_PFN + NPAGES);
    for (int i = 0; i < NPAGES; i++)
        owner[i] = -1;
}

static void snapshot(struct buddy_zone *z, unsigned long long *out)
{
    for (int o = 0; o <= MAX_ORDER; o++)
        out[o] = z->nr_free[o];
}

static void claim(struct buddy_zone *z, struct page *pg, unsigned order, int id)
{
    unsigned long long idx = pg - z->pages;
    CHECK((buddy_pfn(z, pg) & ((1UL << order) - 1)) == 0, "pfn %#llx not aligned to order %u",
          (unsigned long long)buddy_pfn(z, pg), order);
    for (unsigned long long i = idx; i < idx + (1UL << order); i++) {
        CHECK(i < NPAGES, "block runs past the zone");
        CHECK(i < HOLE_LO || i >= HOLE_HI, "allocated reserved page %llu", i);
        CHECK(owner[i] == -1, "page %llu already owned by %d (overlap)", i, owner[i]);
        owner[i] = id;
    }
}

static void unclaim(struct buddy_zone *z, struct page *pg, unsigned order)
{
    unsigned long long idx = pg - z->pages;
    for (unsigned long long i = idx; i < idx + (1UL << order); i++)
        owner[i] = -1;
}

static void test_initial_layout(void)
{
    struct buddy_zone z;
    setup(&z);
    CHECK(z.free_pages == NPAGES - (HOLE_HI - HOLE_LO), "free %llu",
          (unsigned long long)z.free_pages);
    unsigned long long sum = 0;
    for (int o = 0; o <= MAX_ORDER; o++)
        sum += z.nr_free[o] << o;
    CHECK(sum == z.free_pages, "block sum %llu != free %llu", sum,
          (unsigned long long)z.free_pages);
    CHECK(pages[HOLE_LO].flags & PG_RESERVED, "hole not reserved");
}

static void test_exhaust_and_restore(void)
{
    struct buddy_zone z;
    unsigned long long initial[MAX_ORDER + 1], after[MAX_ORDER + 1];
    static struct page *got[NPAGES];
    int n = 0;

    setup(&z);
    snapshot(&z, initial);
    struct page *pg;
    while ((pg = buddy_alloc(&z, 0)) != NULL) {
        claim(&z, pg, 0, n);
        got[n++] = pg;
    }
    CHECK(n == NPAGES - (HOLE_HI - HOLE_LO), "allocated %d pages", n);
    CHECK(z.free_pages == 0, "free_pages %llu after exhaustion", (unsigned long long)z.free_pages);

    /* Free in a shuffled order; everything must coalesce back. */
    srand(1);
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        struct page *t = got[i];
        got[i] = got[j];
        got[j] = t;
    }
    for (int i = 0; i < n; i++) {
        CHECK(buddy_free(&z, got[i], 0) == 0, "free %d failed", i);
        unclaim(&z, got[i], 0);
    }
    snapshot(&z, after);
    CHECK(memcmp(initial, after, sizeof(initial)) == 0, "free lists did not coalesce back");
}

static void test_random(void)
{
    struct buddy_zone z;
    enum { SLOTS = 400 };
    static struct page *blk[SLOTS];
    static unsigned ord[SLOTS];
    unsigned long long initial[MAX_ORDER + 1], after[MAX_ORDER + 1];

    setup(&z);
    snapshot(&z, initial);
    srand(42);
    for (int step = 0; step < 200000; step++) {
        int s = rand() % SLOTS;
        if (blk[s]) {
            unclaim(&z, blk[s], ord[s]);
            CHECK(buddy_free(&z, blk[s], ord[s]) == 0, "free failed");
            blk[s] = NULL;
        } else {
            ord[s] = rand() % 6;
            blk[s] = buddy_alloc(&z, ord[s]);
            if (blk[s])
                claim(&z, blk[s], ord[s], s);
        }
        if (failures > 20)
            return;
    }
    for (int s = 0; s < SLOTS; s++)
        if (blk[s]) {
            unclaim(&z, blk[s], ord[s]);
            buddy_free(&z, blk[s], ord[s]);
            blk[s] = NULL;
        }
    snapshot(&z, after);
    CHECK(memcmp(initial, after, sizeof(initial)) == 0, "free lists did not coalesce back");
}

static void test_errors(void)
{
    struct buddy_zone z;
    setup(&z);

    struct page *pg = buddy_alloc(&z, 2);
    CHECK(pg != NULL, "alloc");
    CHECK(pg[1].flags & PG_TAIL && pg[1].head == pg, "tail pages marked");
    CHECK(buddy_free(&z, &pg[1], 0) == -1, "tail free accepted");
    CHECK(buddy_free(&z, pg, 2) == 0, "valid free");
    CHECK(buddy_free(&z, pg, 2) == -1, "double free accepted");
    CHECK(buddy_free(&z, &pages[HOLE_LO], 0) == -1, "reserved free accepted");
    CHECK(buddy_alloc(&z, MAX_ORDER + 1) == NULL, "oversized order");
}

int main(void)
{
    test_initial_layout();
    test_exhaust_and_restore();
    test_random();
    test_errors();
    printf("buddy: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
