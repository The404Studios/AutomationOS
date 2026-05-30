#include "llm.h"
#include "../libc/string.h"
#include "../libc/stdio.h"
#include "../libc/syscall.h"

static char* read_file_tool(const char* path) {
    static char buf[4096];
    int fd = open(path, 0);
    if (fd < 0) { buf[0] = 0; return buf; }
    long n = read(fd, buf, 4095);
    close(fd);
    if (n < 0) n = 0; buf[n] = 0;
    return buf;
}

static int write_file_tool(const char* path, const char* content) {
    int fd = open(path, 1);
    if (fd < 0) return -1;
    long n = write(fd, content, strlen(content));
    close(fd);
    return (int)n;
}

static int spawn_tool(const char* path) { return spawn(path); }

static char* run_cmd_tool(const char* cmd) {
    static char buf[128];
    int len = strlen(cmd);
    if (len > 120) len = 120;
    memcpy(buf, cmd, len); buf[len] = 0;
    return buf;
}

__attribute__((section(".text")))
void _start(void) {
    printf("[AID] AutomationOS Intelligence Daemon\n");
    printf("[AID] Booting AI agent subsystem...\n\n");

    void* model_addr = 0;
    unsigned long model_size = 0;

    int ret = map_file("/models/qwen.gguf", &model_addr, &model_size);
    if (ret != 0) {
        printf("[AID] No model file found at /models/qwen.gguf\n");
        printf("[AID] Try: place a Qwen2.5-0.5B GGUF file in initrd\n");
        printf("[AID] Falling back to tool-only mode.\n\n");
        goto tool_only;
    }

    printf("[AID] Model mapped at 0x%lx (%lu bytes)\n",
           (unsigned long)model_addr, model_size);

    LlmModel model;
    if (model_load(&model, model_addr, model_size) != 0) {
        printf("[AID] Failed to load model\n");
        goto tool_only;
    }

    printf("[AID] Model loaded\n");

    AgentTools tools;
    tools.read_file = read_file_tool;
    tools.write_file = write_file_tool;
    tools.spawn = spawn_tool;
    tools.run_cmd = run_cmd_tool;

    const char* sysprompt =
        "You are an AI assistant running natively on AutomationOS, "
        "a custom x86_64 operating system. You can read/write files, "
        "spawn processes, and execute commands to automate tasks.";

    agent_loop(&model, &tools, sysprompt);
    model_free(&model);
    return;

tool_only:
    printf("[AID] Available tools:\n");
    printf("  read_file(path)   - Read files\n");
    printf("  write_file(path)  - Write files\n");
    printf("  spawn(path)       - Launch a program\n");
    printf("  run_cmd(cmd)      - Execute a shell command\n\n");
    printf("[AID] Agent loop requires model. Exiting.\n");
    exit(0);
}
