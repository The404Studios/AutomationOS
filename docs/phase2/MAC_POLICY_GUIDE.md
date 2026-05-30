# MAC Policy Administration Guide

**Version:** 1.0  
**Audience:** Security Administrators  
**Date:** 2026-05-26

---

## Table of Contents

1. [Introduction to MAC](#introduction-to-mac)
2. [Security Labels](#security-labels)
3. [Policy Language](#policy-language)
4. [Writing Policy Rules](#writing-policy-rules)
5. [Domain Transitions](#domain-transitions)
6. [Multi-Level Security (MLS)](#multi-level-security-mls)
7. [Policy Management](#policy-management)
8. [Troubleshooting](#troubleshooting)
9. [Policy Examples](#policy-examples)
10. [Best Practices](#best-practices)

---

## Introduction to MAC

Mandatory Access Control (MAC) enforces system-wide security policies that users cannot override. Even if a user owns a file, MAC policy may deny access.

### MAC vs DAC vs Capabilities

| Feature | DAC | Capabilities | MAC |
|---------|-----|--------------|-----|
| User Control | Yes | Limited | No |
| System-Wide Policy | No | No | Yes |
| Fine-Grained | Moderate | High | High |
| Default Behavior | Allow owner | Deny all | Policy-defined |

### Key MAC Concepts

- **Subject**: Process attempting an operation (e.g., web server)
- **Object**: Resource being accessed (e.g., file, socket, process)
- **Label**: Security classification (domain/type + MLS level)
- **Rule**: Policy statement allowing or denying access
- **Domain**: Security context for processes (e.g., `web_server_t`)
- **Type**: Security context for objects (e.g., `etc_t`, `home_t`)

### MAC Modes

**Permissive Mode**:
- Policy violations logged but not enforced
- Use for policy development and testing
- Shows what would be blocked in enforcing mode

**Enforcing Mode**:
- Policy violations blocked and logged
- Production mode
- Processes receive EACCES error on denial

```bash
# Check current mode
macctl --status

# Switch to permissive mode
macctl --mode permissive

# Switch to enforcing mode
macctl --mode enforcing
```

---

## Security Labels

### Label Structure

Every process and object has a security label:

```
domain:type:mls_level:categories
```

**Examples:**

- Process: `web_server_t:user_t:confidential:web`
- File: `file_t:etc_t:secret:admin`
- Socket: `socket_t:http_port_t:unclassified:public`

### Label Components

**Domain/Type** (Required):
- For processes: Security domain (e.g., `user_t`, `system_t`)
- For objects: Object type (e.g., `file_t`, `etc_t`, `home_t`)
- Max length: 64 characters
- Convention: Ends with `_t` suffix

**MLS Level** (Optional):
- Multi-Level Security classification
- Values: `unclassified`, `confidential`, `secret`, `top_secret`
- Default: `unclassified`

**Categories** (Optional):
- Compartments for additional isolation
- Comma-separated list
- Examples: `web`, `database`, `admin`

### Viewing Labels

```bash
# View process label
macctl getlabel -p $PID

# View file label
macctl getlabel -f /path/to/file

# View all labels in directory
macctl getlabel -r /home/user

# Example output:
# Process 42: web_server_t:confidential:web
# File /etc/passwd: etc_t:secret:system
```

### Setting Labels

```bash
# Set process label (requires sys_admin capability)
macctl setlabel -p $PID -l "web_server_t:confidential:web"

# Set file label
macctl setlabel -f /var/www/index.html -l "file_t:unclassified:public"

# Set label recursively on directory
macctl setlabel -r /var/www -l "file_t:unclassified:web"
```

### Default Labels

AutomationOS defines standard security domains:

| Domain | Description | Typical Use |
|--------|-------------|-------------|
| `kernel_t` | Kernel code | N/A (kernel only) |
| `init_t` | Init process | /sbin/init |
| `system_t` | System services | systemd, udevd |
| `user_t` | User processes | User applications |
| `untrusted_t` | Sandboxed apps | Web browser, downloads |
| `isolated_t` | Fully isolated | Malware analysis |

| Type | Description | Typical Paths |
|------|-------------|--------------|
| `file_t` | Generic files | Most files |
| `etc_t` | System configuration | /etc/* |
| `shadow_t` | Sensitive auth | /etc/shadow |
| `bin_t` | Executables | /bin/*, /usr/bin/* |
| `lib_t` | Libraries | /lib/*, /usr/lib/* |
| `dev_t` | Device files | /dev/* |
| `tmp_t` | Temporary files | /tmp/* |
| `home_t` | User files | /home/*/* |

---

## Policy Language

### Policy File Format

MAC policies are written in a domain-specific language:

```
# Comments start with #

# Basic rule syntax:
allow source_domain:target_type:object_type { permissions };

# Deny rule:
deny source_domain:target_type:object_type { permissions };

# Type transition:
transition source_domain:target_type -> result_domain;

# MLS constraint:
mlsconstraint object_type { permissions } expression;
```

### Rule Syntax

**Allow Rule:**

```
allow web_server_t:file_t:file { read write };
```

Components:
- `allow`: Allow access
- `web_server_t`: Source domain (process)
- `file_t`: Target type (object)
- `file`: Object class (file, dir, socket, process, etc.)
- `{ read write }`: Permitted operations

**Deny Rule:**

```
deny untrusted_t:shadow_t:file { all };
```

Deny rules override allow rules.

**Wildcard Rules:**

```
# Allow domain to access all file types
allow user_t:*:file { read };

# Allow all domains to read public files
allow *:public_t:file { read };
```

Use wildcards sparingly.

### Permissions by Object Class

**File Permissions:**

```
{ read write execute append create delete chmod chown }
```

**Directory Permissions:**

```
{ read write search add_entry remove_entry }
```

**Socket Permissions:**

```
{ bind connect listen accept send recv }
```

**Process Permissions:**

```
{ signal ptrace kill fork exec transition }
```

**IPC Permissions:**

```
{ read write create destroy getattr setattr }
```

### Macros

Define reusable rule sets:

```
# Define macro
define allow_web_content(domain) {
    allow domain:file_t:file { read };
    allow domain:www_t:file { read };
    allow domain:www_t:dir { read search };
}

# Use macro
allow_web_content(web_server_t);
allow_web_content(php_fpm_t);
```

---

## Writing Policy Rules

### Step 1: Identify Subject and Object

**Subject**: Process attempting action
**Object**: Resource being accessed

Example: Web server reading HTML file
- Subject: `web_server_t` (domain)
- Object: `/var/www/index.html` with label `www_t` (type)

### Step 2: Determine Required Permissions

What operation is needed?

- Reading file: `{ read }`
- Writing file: `{ write }`
- Executing file: `{ execute }`
- Creating file: `{ create }`
- Listing directory: `{ read search }`

### Step 3: Write the Rule

```
allow web_server_t:www_t:file { read };
allow web_server_t:www_t:dir { read search };
```

### Step 4: Test in Permissive Mode

```bash
# Enable permissive mode
macctl --mode permissive

# Run application
systemctl start myservice

# Check audit log for denials
grep MAC_DENIED /var/log/audit.log

# Example output:
# [MAC_DENIED] web_server_t:www_t:file { read }
```

### Step 5: Add Rules for Denials

Add rules for logged denials:

```
allow web_server_t:www_t:file { read };
```

### Step 6: Enable Enforcing Mode

```bash
# Test application still works
systemctl status myservice

# Enable enforcing mode
macctl --mode enforcing

# Monitor for unexpected denials
tail -f /var/log/audit.log | grep MAC_DENIED
```

---

## Domain Transitions

### What is a Domain Transition?

When a process executes a new program, its security domain can change.

Example: User executing `sudo`
- Before: `user_t` domain
- Execute: `/usr/bin/sudo` (labeled `sudo_exec_t`)
- After: `sudo_t` domain (privileged)

### Transition Rule Syntax

```
transition source_domain:target_type -> result_domain;
```

Example:

```
# User executing sudo transitions to sudo_t
transition user_t:sudo_exec_t -> sudo_t;

# User executing shell stays in user_t
transition user_t:shell_exec_t -> user_t;
```

### Automatic Domain Transitions

Define transitions in policy:

```
# User executing web browser transitions to browser_t (sandboxed)
transition user_t:browser_exec_t -> browser_t;

# Web server spawning CGI script transitions to cgi_t
transition web_server_t:cgi_exec_t -> cgi_t;
```

### Manual Domain Transitions

Process can manually change its domain (requires permission):

```bash
# Process changes its own domain
macctl setdomain -l "restricted_t"

# Requires policy rule:
allow user_t:restricted_t:process { transition };
```

### Preventing Transitions

Deny unwanted transitions:

```
# Untrusted processes cannot transition to privileged domains
deny untrusted_t:sudo_exec_t:process { transition };
deny untrusted_t:su_exec_t:process { transition };
```

---

## Multi-Level Security (MLS)

### MLS Levels

AutomationOS supports four MLS levels:

| Level | Value | Description |
|-------|-------|-------------|
| Unclassified | 0 | Public information |
| Confidential | 1 | Internal use only |
| Secret | 2 | Restricted access |
| Top Secret | 3 | Highly restricted |

### MLS Rules

**Bell-LaPadula Model**:

- **No Read Up**: Subject cannot read objects at higher level
- **No Write Down**: Subject cannot write objects at lower level

**Enforced Automatically**:

```
# Process at level=confidential
# Can read: unclassified, confidential
# Can write: confidential, secret, top_secret
# Cannot read: secret, top_secret
# Cannot write: unclassified
```

### MLS Constraints

Define MLS rules in policy:

```
# Process can only read objects at same or lower level
mlsconstraint file { read } (
    subject_level >= object_level
);

# Process can only write objects at same or higher level
mlsconstraint file { write } (
    subject_level <= object_level
);
```

### Overriding MLS

Process with special permission can violate MLS:

```
# Trusted process can read up and write down
allow trusted_process_t:*:file { mls_override };
```

Use sparingly, only for trusted system services.

### Setting MLS Levels

```bash
# Set process MLS level
macctl setlabel -p $PID -l "user_t:confidential"

# Set file MLS level
macctl setlabel -f /data/classified.txt -l "file_t:secret"
```

### MLS Categories

Refine MLS with categories (compartments):

```bash
# Set process with categories
macctl setlabel -p $PID -l "user_t:secret:finance,hr"

# Process can only access objects with matching categories
# Object labeled: secret:finance  -> Allowed
# Object labeled: secret:hr       -> Allowed
# Object labeled: secret:it       -> Denied
```

---

## Policy Management

### Loading Policies

```bash
# Load policy from file
macctl load-policy /etc/mac/custom.policy

# Load multiple policy files
macctl load-policy /etc/mac/*.policy

# Reload all policies
macctl reload-policy
```

### Policy File Locations

- System policies: `/etc/mac/`
- User policies: `/etc/mac/users/<username>.policy`
- Application policies: `/etc/mac/apps/<appname>.policy`

### Compiling Policies

```bash
# Compile human-readable policy to binary format
mac-compile -i /etc/mac/custom.policy -o /etc/mac/custom.pol

# Load compiled policy
macctl load-policy /etc/mac/custom.pol
```

### Listing Active Rules

```bash
# List all active rules
macctl list-rules

# Filter by source domain
macctl list-rules --source web_server_t

# Filter by target type
macctl list-rules --target www_t

# Filter by object class
macctl list-rules --class file
```

### Removing Rules

```bash
# Remove specific rule
macctl delete-rule --id $RULE_ID

# Remove all rules for domain
macctl delete-rules --source untrusted_t

# Clear all rules (dangerous!)
macctl clear-policy
```

### Policy Versioning

```bash
# Get current policy version
macctl --version

# Export current policy
macctl export-policy > /backup/policy-$(date +%Y%m%d).txt

# Restore policy from backup
macctl load-policy /backup/policy-20260526.txt
```

---

## Troubleshooting

### Diagnosis Tools

```bash
# Check if MAC is enabled
macctl --status

# Check current mode (enforcing/permissive)
macctl --mode

# Check process label
macctl getlabel -p $PID

# Check file label
macctl getlabel -f /path/to/file

# Simulate access check
macctl check --source user_t --target etc_t --class file --perm read
```

### Common Issues

**Issue 1: Access Denied After Policy Change**

Symptom: Application worked before, now gets EACCES

Diagnosis:

```bash
# Check audit log
tail -n 100 /var/log/audit.log | grep MAC_DENIED

# Example output:
# [MAC_DENIED] web_server_t -> etc_t:file { read }
```

Solution:

```bash
# Add missing rule
echo "allow web_server_t:etc_t:file { read };" >> /etc/mac/custom.policy
macctl reload-policy
```

**Issue 2: Domain Transition Not Working**

Symptom: Process stays in same domain after exec

Diagnosis:

```bash
# Check if transition rule exists
macctl list-rules | grep transition

# Check file label
macctl getlabel -f /usr/bin/target-program

# Should have specific exec type (e.g., sudo_exec_t)
```

Solution:

```bash
# Set correct label on executable
macctl setlabel -f /usr/bin/sudo -l "sudo_exec_t"

# Add transition rule
echo "transition user_t:sudo_exec_t -> sudo_t;" >> /etc/mac/transitions.policy
macctl reload-policy
```

**Issue 3: MLS Violation**

Symptom: Process cannot read file despite allow rule

Diagnosis:

```bash
# Check process MLS level
macctl getlabel -p $PID
# Output: user_t:confidential

# Check file MLS level
macctl getlabel -f /data/classified.txt
# Output: file_t:secret

# Problem: Process (confidential) cannot read file (secret) due to MLS
```

Solution (Option 1 - Raise process level):

```bash
macctl setlabel -p $PID -l "user_t:secret"
```

Solution (Option 2 - Lower file level):

```bash
macctl setlabel -f /data/classified.txt -l "file_t:confidential"
```

**Issue 4: Policy Won't Load**

Symptom: `macctl load-policy` fails with error

Diagnosis:

```bash
# Validate policy syntax
mac-compile --check /etc/mac/custom.policy

# Example output:
# Line 15: Syntax error: Expected ';' after rule
```

Solution: Fix syntax errors in policy file

### Testing Policies

```bash
# Test policy in permissive mode first
macctl --mode permissive
# Run application, check logs
tail -f /var/log/audit.log | grep MAC_DENIED

# Fix all denials, then switch to enforcing
macctl --mode enforcing
```

---

## Policy Examples

### Example 1: Web Server Policy

```
# Web server domain definition
domain web_server_t;

# Allow reading web content
allow web_server_t:www_t:file { read };
allow web_server_t:www_t:dir { read search };

# Allow binding to HTTP ports
allow web_server_t:http_port_t:socket { bind listen accept };

# Allow network I/O
allow web_server_t:*:socket { send recv };

# Allow logging
allow web_server_t:log_t:file { write append create };

# Deny everything else (implicit default-deny)
```

### Example 2: Database Server Policy

```
domain database_t;

# Allow data directory access
allow database_t:database_data_t:file { read write create delete };
allow database_t:database_data_t:dir { read write search add_entry remove_entry };

# Allow binding to database port
allow database_t:database_port_t:socket { bind listen accept };

# Allow client connections
allow database_t:*:socket { send recv };

# Allow configuration access
allow database_t:database_conf_t:file { read };

# Deny write to configuration
deny database_t:database_conf_t:file { write };
```

### Example 3: User Application Policy

```
domain user_app_t;

# Allow reading user home directory
allow user_app_t:home_t:file { read };
allow user_app_t:home_t:dir { read search };

# Allow writing to user home directory
allow user_app_t:home_t:file { write create };

# Allow network connections (not binding)
allow user_app_t:*:socket { connect send recv };

# Deny system files
deny user_app_t:etc_t:file { all };
deny user_app_t:bin_t:file { write };
```

### Example 4: Sandboxed Browser Policy

```
domain browser_t;

# Allow reading downloads directory only
allow browser_t:download_t:file { read write create };
allow browser_t:download_t:dir { read write search add_entry };

# Allow network access (HTTP/HTTPS only)
allow browser_t:http_port_t:socket { connect send recv };

# Allow GPU access (for rendering)
allow browser_t:gpu_device_t:device { read write ioctl };

# Deny all other file access
deny browser_t:home_t:file { all };
deny browser_t:etc_t:file { all };
deny browser_t:dev_t:device { all };

# Deny process interaction
deny browser_t:*:process { signal ptrace };
```

### Example 5: System Service Policy

```
domain system_service_t;

# Allow reading system configuration
allow system_service_t:etc_t:file { read };

# Allow writing to service-specific directory
allow system_service_t:service_var_t:file { read write create delete };

# Allow IPC with other system services
allow system_service_t:system_service_t:ipc { read write };

# Allow logging
allow system_service_t:syslog_t:socket { send };
```

### Example 6: Container Policy

```
domain container_t;

# Allow access only to container root
allow container_t:container_file_t:file { read write execute };
allow container_t:container_file_t:dir { read write search add_entry remove_entry };

# Isolated network namespace (defined elsewhere)
allow container_t:container_net_t:socket { bind connect listen accept send recv };

# Deny host filesystem access
deny container_t:file_t:file { all };
deny container_t:home_t:file { all };

# Deny other container access
deny container_t:container_file_t:file { all }
    unless (container_id == self.container_id);
```

---

## Best Practices

### Policy Development Workflow

1. **Start in Permissive Mode**: Develop policies without blocking access
2. **Monitor Audit Logs**: Identify all required accesses
3. **Write Minimal Rules**: Grant only necessary permissions
4. **Test Thoroughly**: Run application through all use cases
5. **Enable Enforcing Mode**: Switch to enforcement
6. **Monitor Production**: Watch for unexpected denials

### Principle of Least Privilege

```
# BAD: Overly broad rule
allow web_server_t:*:file { all };

# GOOD: Specific rule
allow web_server_t:www_t:file { read };
allow web_server_t:www_t:dir { read search };
```

### Use Explicit Denies

```
# Explicitly deny sensitive access
deny untrusted_t:shadow_t:file { all };
deny untrusted_t:ssh_key_t:file { all };

# Even if allow rule exists, deny overrides
```

### Organize Policies by Domain

```
/etc/mac/
├── base.policy          # Core system rules
├── users.policy         # User application rules
├── services/
│   ├── web.policy       # Web server rules
│   ├── database.policy  # Database rules
│   └── ssh.policy       # SSH daemon rules
└── apps/
    ├── browser.policy   # Browser sandbox rules
    └── office.policy    # Office suite rules
```

### Document Policy Decisions

```
# Good: Include rationale
# Web server needs to read configuration files in /etc/webapp/
allow web_server_t:etc_t:file { read };

# Bad: No explanation
allow web_server_t:etc_t:file { read };
```

### Regular Policy Audits

```bash
# Find overly broad rules
macctl list-rules | grep "allow.*:.*:.*{ all }"

# Find rules with wildcards
macctl list-rules | grep "allow \*:"

# Review denied accesses
grep MAC_DENIED /var/log/audit.log | sort | uniq -c | sort -rn
```

### Version Control

```bash
# Store policies in git
cd /etc/mac
git init
git add *.policy
git commit -m "Initial MAC policy"

# Track changes
git diff custom.policy

# Rollback if needed
git checkout HEAD~1 custom.policy
macctl reload-policy
```

---

## Advanced Topics

### Context-Dependent Rules

Allow access only under certain conditions:

```
# Allow database access only from specific host
allow app_t:database_t:socket { connect }
    where (source_ip == "10.0.0.5");

# Allow file write only during business hours
allow user_t:file_t:file { write }
    where (hour >= 9 && hour <= 17);
```

### Policy Debugging

```bash
# Enable verbose MAC logging
macctl --debug on

# Trace policy evaluation for specific access
macctl trace --source user_t --target etc_t --class file --perm read

# Output shows rule evaluation:
# Checking rule 1: allow user_t:etc_t:file { read } -> MATCH
# Access: ALLOWED
```

### Performance Optimization

```bash
# Cache policy decisions (faster, but stale after policy reload)
macctl --enable-cache

# Disable auditing for allowed accesses (reduces log volume)
macctl --audit-denied-only

# Profile policy evaluation time
macctl --profile
```

---

## Additional Resources

- **Security Architecture**: `docs/phase2/SECURITY_ARCHITECTURE.md`
- **Capability Guide**: `docs/phase2/CAPABILITY_ADMIN_GUIDE.md`
- **Audit Guide**: `docs/phase2/AUDIT_GUIDE.md`
- **API Reference**: `docs/phase2/MAC_API.md`
- **Man Pages**: `man 8 macctl`, `man 5 mac-policy`

---

**Document Version:** 1.0  
**Last Updated:** 2026-05-26  
**Maintained By:** AutomationOS Security Team
