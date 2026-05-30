# Manual Testing Checklist for Minimal Terminal

## Build Test

- [ ] Run `make` or `./build.sh`
- [ ] Verify no compilation errors
- [ ] Verify binary is created: `../../build/userspace/terminal/terminal`
- [ ] Check binary size (should be ~50-100KB)

## Startup Test

- [ ] Run `./build/userspace/terminal/terminal`
- [ ] Verify "Terminal started" message appears
- [ ] Verify initial prompt (`$ `) is displayed
- [ ] Verify window opens (when window manager integrated)

## Input Test

### Character Input
- [ ] Type `a` - should appear on screen
- [ ] Type `hello` - should appear on screen
- [ ] Type special chars: `!@#$%^&*()` - should appear

### Backspace
- [ ] Type `hello`
- [ ] Press backspace 2 times
- [ ] Should show `hel`
- [ ] Cursor should move back

### Enter Key
- [ ] Type `help`
- [ ] Press Enter
- [ ] Should execute and show command list
- [ ] New prompt should appear

## Command Test

### help
```
$ help
Available commands:
  echo - Print text to screen
  ls - List files
  clear - Clear screen
  help - Show available commands
  exit - Exit terminal
$ 
```

### echo
```
$ echo hello world
hello world
$ 
```

```
$ echo testing 1 2 3
testing 1 2 3
$ 
```

### ls
```
$ ls
bin/
etc/
home/
tmp/
usr/
var/
$ 
```

### clear
```
$ clear
[Screen should clear]
$ 
```

### exit
```
$ exit
Goodbye!
[Terminal should exit]
```

### Unknown Command
```
$ foobar
Unknown command: foobar
$ 
```

## Scrolling Test

1. Type and execute commands until buffer fills (25+ lines)
2. Verify screen scrolls automatically
3. Old lines should disappear at top
4. New lines appear at bottom
5. Cursor should stay visible

### Test Script
```bash
$ echo line 1
$ echo line 2
$ echo line 3
... (continue until line 30)
```

Bottom lines should remain visible.

## Edge Cases

### Empty Command
- [ ] Press Enter without typing
- [ ] Should show new prompt without error

### Long Command
- [ ] Type 100+ characters
- [ ] Should handle gracefully (truncate or wrap)

### Rapid Input
- [ ] Type very quickly
- [ ] All characters should appear
- [ ] No dropped input

### Special Keys
- [ ] Ctrl+D - should exit
- [ ] Backspace at start - should do nothing
- [ ] Multiple spaces - should be preserved in echo

## Visual Test

### Font Rendering
- [ ] All letters (a-z, A-Z) render correctly
- [ ] All digits (0-9) render correctly
- [ ] All symbols (!@#$%^&*()_+-=[]{}|;:',.<>?/) render correctly
- [ ] Characters are legible
- [ ] No overlapping characters
- [ ] Proper spacing between characters

### Colors
- [ ] Background is black
- [ ] Text is white
- [ ] Cursor is visible (white underline/block)
- [ ] Good contrast

### Layout
- [ ] 80 characters fit horizontally
- [ ] 25 lines fit vertically
- [ ] No text cutoff at edges
- [ ] Cursor visible at all times

## Performance Test

- [ ] Terminal responds immediately to input
- [ ] No lag when typing
- [ ] Scrolling is smooth
- [ ] No screen flicker
- [ ] CPU usage is low when idle

## Exit Test

### Normal Exit
- [ ] Type `exit` + Enter
- [ ] Terminal exits cleanly
- [ ] No error messages

### Ctrl+D Exit
- [ ] Press Ctrl+D
- [ ] Terminal exits cleanly
- [ ] No error messages

## Integration Test (When WM Available)

### Window
- [ ] Window appears on screen
- [ ] Window has title "Terminal"
- [ ] Window size is 800x600
- [ ] Window can be moved (if WM supports)
- [ ] Window can be closed

### Events
- [ ] Keyboard events work
- [ ] Focus events work
- [ ] Window close button works

## Test Results

Date: ___________
Tester: ___________

Pass: ___ / ___
Fail: ___ / ___

Notes:
_______________________________________________
_______________________________________________
_______________________________________________
