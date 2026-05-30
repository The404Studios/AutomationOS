// AutomationOS IDE - Blueprint Visual Editor Component
#ifndef IDE_BLUEPRINT_H
#define IDE_BLUEPRINT_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_NODES 512
#define MAX_CONNECTIONS 1024
#define MAX_NODE_NAME 64
#define MAX_NODE_INPUTS 16
#define MAX_NODE_OUTPUTS 16

// Node types
typedef enum {
    NODE_FUNCTION,
    NODE_VARIABLE,
    NODE_BRANCH,
    NODE_LOOP,
    NODE_EVENT,
    NODE_COMMENT,
    NODE_MACRO,
    NODE_CUSTOM
} node_type_t;

// Pin types
typedef enum {
    PIN_EXEC,      // Execution flow
    PIN_INT,       // Integer
    PIN_FLOAT,     // Float
    PIN_STRING,    // String
    PIN_BOOL,      // Boolean
    PIN_POINTER,   // Pointer/Reference
    PIN_STRUCT,    // Structure
    PIN_ARRAY      // Array
} pin_type_t;

// Pin definition
typedef struct {
    char name[MAX_NODE_NAME];
    pin_type_t type;
    void *default_value;
    bool connected;
} pin_t;

// Blueprint node
typedef struct {
    uint32_t id;
    node_type_t type;
    char name[MAX_NODE_NAME];

    int x, y;  // Position on canvas
    int width, height;

    pin_t inputs[MAX_NODE_INPUTS];
    pin_t outputs[MAX_NODE_OUTPUTS];
    int input_count;
    int output_count;

    void *user_data;  // Node-specific data
    bool selected;
} blueprint_node_t;

// Node connection
typedef struct {
    uint32_t id;
    uint32_t source_node;
    int source_pin;
    uint32_t target_node;
    int target_pin;

    bool selected;
} blueprint_connection_t;

// Blueprint graph
typedef struct {
    char name[MAX_NODE_NAME];
    blueprint_node_t *nodes[MAX_NODES];
    blueprint_connection_t *connections[MAX_CONNECTIONS];
    int node_count;
    int connection_count;

    // Canvas state
    int viewport_x, viewport_y;
    float zoom;

    // Selection
    uint32_t *selected_nodes;
    int selected_count;

    bool modified;
} blueprint_graph_t;

// Blueprint editor state
struct blueprint_editor {
    blueprint_graph_t *graphs[32];
    int graph_count;
    int active_graph;

    // Editor state
    bool grid_snap;
    int grid_size;
    bool show_minimap;

    // Mouse state
    int mouse_x, mouse_y;
    bool dragging;
    bool panning;
};

// Blueprint operations
blueprint_graph_t* blueprint_create_graph(const char *name);
void blueprint_destroy_graph(blueprint_graph_t *graph);

// Node operations
blueprint_node_t* blueprint_add_node(blueprint_graph_t *graph, node_type_t type, int x, int y);
void blueprint_remove_node(blueprint_graph_t *graph, uint32_t node_id);
void blueprint_move_node(blueprint_graph_t *graph, uint32_t node_id, int dx, int dy);

// Connection operations
blueprint_connection_t* blueprint_connect(blueprint_graph_t *graph,
                                          uint32_t src_node, int src_pin,
                                          uint32_t dst_node, int dst_pin);
void blueprint_disconnect(blueprint_graph_t *graph, uint32_t connection_id);

// Selection operations
void blueprint_select_node(blueprint_graph_t *graph, uint32_t node_id, bool multi);
void blueprint_deselect_all(blueprint_graph_t *graph);
void blueprint_delete_selected(blueprint_graph_t *graph);

// File operations
int blueprint_load(blueprint_graph_t *graph, const char *path);
int blueprint_save(blueprint_graph_t *graph, const char *path);

// Code generation
int blueprint_compile_to_c(blueprint_graph_t *graph, const char *output_path);
int blueprint_validate(blueprint_graph_t *graph);

// Rendering
void blueprint_render(struct blueprint_editor *editor);
void blueprint_render_node(blueprint_node_t *node);
void blueprint_render_connection(blueprint_connection_t *conn);

// Interaction
blueprint_node_t* blueprint_get_node_at(blueprint_graph_t *graph, int x, int y);
int blueprint_get_pin_at(blueprint_node_t *node, int x, int y, bool is_input);

#endif // IDE_BLUEPRINT_H
