/* cc regression: a packed char run keeps each char at consecutive 1-byte
 * offsets (CC-STRUCTINIT-0). EXPECT (s.a + s.b): 131.
 * Both fields must emit `db` (a at offset 0, b at offset 1) -- emitting `dq`
 * would push b to offset 8 and read it back as 0. */
struct C { char a; char b; };
struct C s = {65, 66};

int main(void) {
    return s.a + s.b;
}
