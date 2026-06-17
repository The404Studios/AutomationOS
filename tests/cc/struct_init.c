/* cc regression: a global struct brace-initializer must be emitted by layout
 * (CC-STRUCTINIT-0). EXPECT (p.x + p.y): 33.
 * Pre-fix symptom: the {11,22} init-list was dropped -> a single `dq 0`, so p
 * occupied 8 bytes and p.y read past it. */
struct P { int x; int y; };
struct P p = {11, 22};

int main(void) {
    return p.x + p.y;
}
