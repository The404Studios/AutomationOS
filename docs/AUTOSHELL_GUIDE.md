# AutoShell User Guide

## Table of Contents

1. [Introduction](#introduction)
2. [Installation](#installation)
3. [Basic Usage](#basic-usage)
4. [Features](#features)
5. [Built-in Commands](#built-in-commands)
6. [Shell Scripting](#shell-scripting)
7. [Tab Completion](#tab-completion)
8. [History](#history)
9. [Job Control](#job-control)
10. [Aliases and Functions](#aliases-and-functions)
11. [Environment Variables](#environment-variables)
12. [Automation Scripts](#automation-scripts)
13. [Advanced Features](#advanced-features)
14. [Troubleshooting](#troubleshooting)

---

## Introduction

AutoShell is a modern, feature-rich command-line shell for AutomationOS. It provides an intuitive interface for system administration, automation, and scripting with powerful features including:

- **Command execution** with pipes and redirections
- **Job control** (background/foreground processes)
- **Command history** with search and navigation
- **Tab completion** for commands, files, and variables
- **Shell scripting** with full programming constructs
- **Aliases and functions** for command customization
- **Environment management** with variable expansion

### Why AutoShell?

- **Productivity**: Fast command entry with intelligent completion
- **Automation**: Powerful scripting capabilities
- **Customization**: Aliases, functions, and environment variables
- **User-friendly**: Intuitive interface with helpful prompts

---

## Installation

### Building from Source

```bash
cd userspace/bin
gcc -o autoshell autoshell.c autoshell_builtins.c autoshell_script.c completion.c -lreadline
sudo install -m 755 autoshell /usr/local/bin/
```

### Setting as Default Shell

```bash
# Add to /etc/shells
echo "/usr/local/bin/autoshell" | sudo tee -a /etc/shells

# Change your default shell
chsh -s /usr/local/bin/autoshell
```

---

## Basic Usage

### Starting AutoShell

Simply type `autoshell` at your current shell prompt:

```bash
$ autoshell

═══════════════════════════════════════════
  AutoShell v1.0.0 - AutomationOS Shell  
═══════════════════════════════════════════

Type 'help' for built-in commands.
Type 'exit' to quit.

autoshell>
```

### Basic Commands

```bash
# Print working directory
autoshell> pwd
/home/user

# Change directory
autoshell> cd /var/log

# List files
autoshell> ls -la

# Run a program
autoshell> vim myfile.txt

# Exit shell
autoshell> exit
```

---

## Features

### 1. Command Execution

Execute any program in your PATH:

```bash
autoshell> ls -l /home
autoshell> grep pattern file.txt
autoshell> python3 script.py
```

### 2. Pipes and Redirections

Chain commands together:

```bash
# Pipe output
autoshell> ls -l | grep txt | wc -l

# Redirect output
autoshell> echo "Hello" > file.txt

# Append output
autoshell> echo "World" >> file.txt

# Redirect input
autoshell> wc -l < file.txt

# Redirect both
autoshell> command > output.txt 2> error.txt
```

### 3. Background Jobs

Run commands in background:

```bash
# Start background job
autoshell> long_running_command &
[1] 12345

# List jobs
autoshell> jobs
[1] + Running    long_running_command

# Bring to foreground
autoshell> fg %1

# Send to background (after Ctrl+Z)
autoshell> bg %1
```

### 4. Command History

Navigate and search command history:

```bash
# View history
autoshell> history

# Search history
autoshell> history -s grep

# Repeat last command
autoshell> !!

# Repeat command #42
autoshell> !42

# Use Ctrl+R for reverse search
(reverse-i-search): grep
```

### 5. Tab Completion

Press Tab to complete:

- Command names
- File and directory paths
- Environment variables
- Aliases

```bash
# Complete command
autoshell> gre<TAB>  → grep

# Complete file
autoshell> cat /etc/pas<TAB>  → /etc/passwd

# Complete variable
autoshell> echo $HO<TAB>  → $HOME
```

---

## Built-in Commands

### Navigation

#### cd - Change Directory

```bash
cd [directory]

# Change to home
autoshell> cd
autoshell> cd ~

# Change to previous directory
autoshell> cd -

# Change to parent
autoshell> cd ..

# Change to absolute path
autoshell> cd /var/log
```

#### pwd - Print Working Directory

```bash
autoshell> pwd
/home/user/projects
```

### Environment

#### export - Set Environment Variable

```bash
export NAME=value

# Examples
autoshell> export PATH=/usr/local/bin:$PATH
autoshell> export EDITOR=vim
autoshell> export DEBUG=1

# View all exports
autoshell> export
```

#### unset - Remove Environment Variable

```bash
autoshell> unset DEBUG
```

#### set - Shell Options

```bash
# View all variables
autoshell> set

# Set shell options
autoshell> set -e  # Exit on error
autoshell> set +e  # Don't exit on error
```

#### env - Display Environment

```bash
autoshell> env
```

### History

#### history - Command History

```bash
# View history
autoshell> history

# View last 10 commands
autoshell> history 10

# Clear history
autoshell> history -c

# Search history
autoshell> history -s pattern
```

### Job Control

#### jobs - List Jobs

```bash
autoshell> jobs
[1] + Running    command1 &
[2] - Stopped    command2
```

#### fg - Foreground Job

```bash
# Bring job to foreground
autoshell> fg %1
```

#### bg - Background Job

```bash
# Resume job in background
autoshell> bg %1
```

#### kill - Send Signal

```bash
# Terminate process
autoshell> kill 12345

# Send specific signal
autoshell> kill -9 12345
autoshell> kill -SIGKILL 12345
```

### Aliases and Functions

#### alias - Create Alias

```bash
# Create alias
autoshell> alias ll='ls -l'
autoshell> alias ..='cd ..'
autoshell> alias grep='grep --color=auto'

# View all aliases
autoshell> alias
```

#### unalias - Remove Alias

```bash
autoshell> unalias ll
```

#### function - Define Function

```bash
autoshell> function greet() {
    echo "Hello, $1!"
}

autoshell> greet World
Hello, World!
```

### Utilities

#### echo - Print Text

```bash
# Simple print
autoshell> echo "Hello, World!"

# No newline
autoshell> echo -n "Prompt: "

# With variables
autoshell> echo "User: $USER"
```

#### printf - Formatted Print

```bash
autoshell> printf "Name: %s\nAge: %d\n" "Alice" 30
Name: Alice
Age: 30
```

#### read - Read Input

```bash
autoshell> read NAME
Alice
autoshell> echo $NAME
Alice
```

#### test / [ - Conditional Test

```bash
# File tests
if [ -f file.txt ]; then
    echo "File exists"
fi

# String tests
if [ "$var" = "value" ]; then
    echo "Match"
fi

# Numeric tests
if [ $count -gt 10 ]; then
    echo "Greater than 10"
fi
```

#### help - Command Help

```bash
# General help
autoshell> help

# Specific command help
autoshell> help cd
autoshell> help export
```

#### type - Show Command Type

```bash
autoshell> type ls
ls is /usr/bin/ls

autoshell> type cd
cd is a shell builtin
```

#### which - Locate Command

```bash
autoshell> which python
/usr/bin/python
```

#### time - Time Command

```bash
autoshell> time ls -R /

real    0.523s
user    0.102s
sys     0.421s
```

---

## Shell Scripting

### Script Structure

Create a shell script:

```bash
#!/usr/bin/autoshell
#
# My Script
#

echo "Starting script..."

# Your commands here

exit 0
```

Make it executable:

```bash
chmod +x script.sh
./script.sh
```

### Variables

```bash
# Define variables
NAME="Alice"
AGE=30
DEBUG=1

# Use variables
echo "Name: $NAME"
echo "Age: $AGE"

# Read-only variables
readonly VERSION="1.0.0"

# Special variables
echo "Script name: $0"
echo "First argument: $1"
echo "All arguments: $@"
echo "Argument count: $#"
echo "Exit status: $?"
echo "Process ID: $$"
```

### Conditionals

#### if Statement

```bash
if [ condition ]; then
    # commands
elif [ condition2 ]; then
    # commands
else
    # commands
fi
```

Examples:

```bash
# File tests
if [ -f "file.txt" ]; then
    echo "File exists"
fi

# String comparison
if [ "$VAR" = "value" ]; then
    echo "Match"
fi

# Numeric comparison
if [ $COUNT -gt 10 ]; then
    echo "Count is greater than 10"
fi

# Multiple conditions
if [ -f "file.txt" ] && [ -r "file.txt" ]; then
    echo "File exists and is readable"
fi
```

#### case Statement

```bash
case "$VAR" in
    pattern1)
        # commands
        ;;
    pattern2)
        # commands
        ;;
    *)
        # default
        ;;
esac
```

Example:

```bash
case "$OSTYPE" in
    linux*)
        echo "Running on Linux"
        ;;
    darwin*)
        echo "Running on macOS"
        ;;
    *)
        echo "Unknown OS"
        ;;
esac
```

### Loops

#### for Loop

```bash
# Iterate over list
for item in apple orange banana; do
    echo "Fruit: $item"
done

# Iterate over files
for file in *.txt; do
    echo "Processing $file"
done

# C-style for loop
for ((i=0; i<10; i++)); do
    echo "Number: $i"
done
```

#### while Loop

```bash
counter=0
while [ $counter -lt 10 ]; do
    echo "Counter: $counter"
    counter=$((counter + 1))
done
```

#### until Loop

```bash
counter=0
until [ $counter -ge 10 ]; do
    echo "Counter: $counter"
    counter=$((counter + 1))
done
```

#### Loop Control

```bash
# break - exit loop
for i in 1 2 3 4 5; do
    if [ $i -eq 3 ]; then
        break
    fi
    echo $i
done

# continue - skip iteration
for i in 1 2 3 4 5; do
    if [ $i -eq 3 ]; then
        continue
    fi
    echo $i
done
```

### Functions

```bash
# Define function
function my_function() {
    echo "Arguments: $@"
    echo "First argument: $1"
    echo "Second argument: $2"
    
    # Return value
    return 0
}

# Call function
my_function arg1 arg2

# Check return value
if my_function; then
    echo "Function succeeded"
fi
```

### Command Substitution

```bash
# Capture command output
current_date=$(date)
echo "Today is $current_date"

# Legacy syntax
current_date=`date`

# Use in conditionals
if [ $(id -u) -eq 0 ]; then
    echo "Running as root"
fi
```

### Arithmetic

```bash
# Arithmetic expansion
result=$((5 + 3))
echo $result  # 8

result=$((10 * 2))
echo $result  # 20

# Increment
counter=0
counter=$((counter + 1))

# let command
let "result = 5 + 3"
echo $result
```

---

## Tab Completion

AutoShell provides intelligent tab completion for:

### Command Completion

```bash
# Type partial command and press Tab
autoshell> gre<TAB>
# Completes to: grep

# Multiple matches
autoshell> g<TAB><TAB>
gcc   gdb   git   grep   gunzip   gzip
```

### File Completion

```bash
# Complete file paths
autoshell> cat /etc/pas<TAB>
# Completes to: /etc/passwd

# Complete in current directory
autoshell> ls Doc<TAB>
# Completes to: Documents/
```

### Variable Completion

```bash
# Complete environment variables
autoshell> echo $HO<TAB>
# Completes to: $HOME

autoshell> echo $PA<TAB>
# Completes to: $PATH
```

### Alias Completion

```bash
# Complete aliases
autoshell> ll<TAB>
# Completes to alias: ls -l
```

---

## History

### Navigation

- **↑ (Up arrow)**: Previous command
- **↓ (Down arrow)**: Next command
- **Ctrl+R**: Reverse search through history
- **Ctrl+S**: Forward search through history

### History Commands

```bash
# View history
autoshell> history

# View last N commands
autoshell> history 20

# Search history
autoshell> history -s grep

# Clear history
autoshell> history -c
```

### History Expansion

```bash
# Repeat last command
autoshell> !!

# Repeat command N
autoshell> !42

# Repeat last command starting with string
autoshell> !grep

# Repeat Nth previous command
autoshell> !-2
```

### History File

History is saved in `~/.autoshell_history`.

---

## Job Control

### Running Background Jobs

```bash
# Start job in background
autoshell> command &
[1] 12345

# Start job, then background it
autoshell> command
^Z  # Press Ctrl+Z to suspend
[1]+  Stopped    command
autoshell> bg %1
[1]+ command &
```

### Managing Jobs

```bash
# List jobs
autoshell> jobs
[1] - Running    job1 &
[2] + Stopped    job2

# Foreground job
autoshell> fg %1

# Background job
autoshell> bg %2

# Kill job
autoshell> kill %1
```

---

## Aliases and Functions

### Creating Aliases

```bash
# Simple alias
autoshell> alias ll='ls -l'

# Alias with pipes
autoshell> alias psgrep='ps aux | grep'

# Alias for directory navigation
autoshell> alias ..='cd ..'
autoshell> alias ...='cd ../..'

# Colored output
autoshell> alias grep='grep --color=auto'
autoshell> alias ls='ls --color=auto'
```

### Common Aliases

```bash
alias ll='ls -lh'
alias la='ls -lha'
alias l='ls -CF'
alias ..='cd ..'
alias ...='cd ../..'
alias grep='grep --color=auto'
alias mkdir='mkdir -pv'
alias h='history'
alias c='clear'
```

### Creating Functions

```bash
# Extract archives
function extract() {
    if [ -f "$1" ]; then
        case "$1" in
            *.tar.gz)  tar xzf "$1"   ;;
            *.tar.bz2) tar xjf "$1"   ;;
            *.zip)     unzip "$1"     ;;
            *.rar)     unrar x "$1"   ;;
            *)         echo "Unknown archive format" ;;
        esac
    else
        echo "File not found: $1"
    fi
}

# Make directory and cd into it
function mkcd() {
    mkdir -p "$1" && cd "$1"
}

# Quick backup
function backup() {
    cp "$1" "$1.backup-$(date +%Y%m%d-%H%M%S)"
}
```

---

## Environment Variables

### Common Variables

```bash
$HOME       # User home directory
$USER       # Current username
$PWD        # Present working directory
$OLDPWD     # Previous working directory
$PATH       # Command search path
$SHELL      # Current shell
$EDITOR     # Default text editor
$LANG       # Locale setting
```

### Setting Variables

```bash
# Session variable
autoshell> VAR=value

# Exported variable (available to child processes)
autoshell> export VAR=value

# Read-only variable
autoshell> readonly VAR=value
```

### Unsetting Variables

```bash
autoshell> unset VAR
```

### PATH Management

```bash
# Add directory to PATH
export PATH="/custom/bin:$PATH"

# Append to PATH
export PATH="$PATH:/custom/bin"

# View PATH
echo $PATH
```

---

## Automation Scripts

AutoShell comes with powerful automation scripts located in `/usr/share/scripts/`.

### System Backup

```bash
# Full backup
/usr/share/scripts/backup.sh -f

# Incremental backup
/usr/share/scripts/backup.sh

# Custom destination
/usr/share/scripts/backup.sh -d /mnt/backup

# Keep 30 days of backups
/usr/share/scripts/backup.sh -r 30
```

### System Monitoring

```bash
# Start monitoring
/usr/share/scripts/monitor.sh

# Custom interval (10 seconds)
/usr/share/scripts/monitor.sh -i 10

# High CPU threshold (90%)
/usr/share/scripts/monitor.sh -t 90

# With email alerts
/usr/share/scripts/monitor.sh -a "mail -s 'Alert' admin@example.com"

# One-shot monitoring
/usr/share/scripts/monitor.sh -o
```

### Application Installer

```bash
# Install package
/usr/share/scripts/install-app.sh install nginx

# Remove package
/usr/share/scripts/install-app.sh remove nginx

# Update package
/usr/share/scripts/install-app.sh update nginx

# Search packages
/usr/share/scripts/install-app.sh search web

# List installed
/usr/share/scripts/install-app.sh list

# Package info
/usr/share/scripts/install-app.sh info nginx
```

---

## Advanced Features

### Redirection

```bash
# Redirect stdout
command > file.txt

# Redirect stderr
command 2> error.txt

# Redirect both
command > output.txt 2>&1

# Append
command >> file.txt

# Here document
cat << EOF > file.txt
Line 1
Line 2
EOF

# Here string
command <<< "input string"
```

### Process Substitution

```bash
# Compare output of two commands
diff <(command1) <(command2)

# Read from process
while read line; do
    echo "$line"
done < <(command)
```

### Brace Expansion

```bash
# Create multiple files
touch file{1,2,3}.txt

# Create range
echo {1..10}

# Character range
echo {a..z}

# Nested expansion
mkdir -p project/{src,bin,doc}/{main,test}
```

### Pattern Matching

```bash
# Glob patterns
*.txt           # All .txt files
file?.txt       # file1.txt, fileA.txt, etc.
file[0-9].txt   # file0.txt through file9.txt

# Extended glob (if enabled)
@(pattern)      # Match exactly one
*(pattern)      # Match zero or more
+(pattern)      # Match one or more
?(pattern)      # Match zero or one
!(pattern)      # Match anything except pattern
```

---

## Troubleshooting

### Command Not Found

```bash
# Check if command exists
autoshell> which command

# Check PATH
autoshell> echo $PATH

# Add directory to PATH
autoshell> export PATH=/custom/bin:$PATH
```

### Permission Denied

```bash
# Check file permissions
autoshell> ls -l file.sh

# Make executable
autoshell> chmod +x file.sh

# Run with sudo
autoshell> sudo ./script.sh
```

### Script Debugging

```bash
# Enable debug mode
autoshell> set -x

# Run script with debugging
autoshell> bash -x script.sh

# Add debug output in script
#!/usr/bin/autoshell
set -x  # Print commands
set -e  # Exit on error
```

### History Issues

```bash
# History not saving
# Check history file permissions
ls -la ~/.autoshell_history

# Fix permissions
chmod 600 ~/.autoshell_history

# Clear corrupted history
history -c
```

---

## Configuration

### Shell Configuration File

Create `~/.autoshellrc`:

```bash
# AutoShell configuration

# Aliases
alias ll='ls -lh'
alias la='ls -lha'
alias ..='cd ..'

# Environment
export EDITOR=vim
export PATH=/custom/bin:$PATH

# Prompt
export PS1="[\u@\h \W]$ "

# History
export HISTSIZE=10000
export HISTFILESIZE=10000

# Functions
function backup() {
    cp "$1" "$1.backup-$(date +%Y%m%d)"
}
```

Source it automatically by adding to shell startup.

---

## Best Practices

1. **Use meaningful variable names**: `$USER_COUNT` instead of `$UC`
2. **Quote variables**: `"$VAR"` to handle spaces
3. **Check command success**: `command || handle_error`
4. **Use functions**: Organize reusable code
5. **Comment your scripts**: Explain complex logic
6. **Handle errors**: Use `set -e` or check return values
7. **Use absolute paths**: In scripts for reliability
8. **Test incrementally**: Test each part before combining

---

## Resources

- **Man pages**: `man bash` for bash compatibility
- **Help command**: `help <command>` for built-in help
- **Scripts**: `/usr/share/scripts/` for examples
- **Community**: AutomationOS forums and documentation

---

*AutoShell v1.0.0 - AutomationOS*
*For updates and support, visit: https://automationos.org*
