/* cc regression: global array brace-initializer + indexing (B4 / CC-ARRAY-INITLIST-0).
 * EXPECT: 40
 * Pre-fix symptom: the {10,20,30} init-list is dropped (array zeroed) and/or the global
 * subscript uses the wrong element size, so a[0]+a[2] is not 40. */
int a[3] = {10, 20, 30};

int main(void) {
    return a[0] + a[2];
}
