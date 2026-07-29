/*
 * kg_populate.c -- Knowledge Graph population tool
 *
 * Models the knowledge graph discovery and construction workflow.
 * Compile: gcc -O2 tools/kg_populate.c src/knowledge_graph.c -I src -o build/kg_populate
 * Run: ./build/kg_populate <memory_data_dir>
 */
#include "knowledge_graph.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "memory_data";
    KnowledgeGraph kg;
    memset(&kg, 0, sizeof(kg));

    /* Notable persons (ent=21) */
    kg_add_node(&kg, "dynasty_a", 0);
    kg_add_node(&kg, "calligraphy", 0);
    kg_add_node(&kg, "artist_1", 21);
    kg_add_node(&kg, "emperor_1", 21);
    kg_add_node(&kg, "style_x", 0);

    kg_add_triple(&kg, "artist_1", REL_DYNASTY, "dynasty_a", 1.0f);
    kg_add_triple(&kg, "emperor_1", REL_DYNASTY, "dynasty_a", 1.0f);
    kg_add_triple(&kg, "artist_1", REL_IDENTITY, "painter", 1.0f);
    kg_add_triple(&kg, "artist_1", REL_IDENTITY, "calligraphy", 0.8f);
    kg_add_triple(&kg, "emperor_1", REL_INVENTED, "style_x", 1.0f);

    /* Toys (ent=22) */
    kg_add_node(&kg, "item_a", 22);
    kg_add_node(&kg, "item_b", 22);

    kg_add_triple(&kg, "item_a", REL_CATEGORY, "toy", 1.0f);
    kg_add_triple(&kg, "item_b", REL_CATEGORY, "toy", 1.0f);
    kg_add_triple(&kg, "item_b", REL_SIMILAR, "item_a", 0.5f);

    /* Appliances (ent=23) */
    kg_add_node(&kg, "item_c", 23);
    kg_add_triple(&kg, "item_c", REL_USED_FOR, "function_x", 1.0f);
    kg_add_triple(&kg, "item_c", REL_USED_FOR, "function_y", 0.8f);

    /* Network devices (ent=24) */
    kg_add_node(&kg, "device_a", 24);
    kg_add_triple(&kg, "device_a", REL_PART_OF, "network_equipment", 1.0f);

    /* Save */
    char path[1024];
    snprintf(path, sizeof(path), "%s/kg_data.bin", dir);
    if (kg_save(&kg, path) == 0) {
        printf("Knowledge graph saved to %s\n", path);
        printf("  Nodes: %u\n", kg.node_count);
        printf("  Triples: %u\n", kg.triple_count);
    } else {
        fprintf(stderr, "Save failed\n");
        return 1;
    }
    return 0;
}
