#include <stdio.h> 
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include "memory.h"

#define MAX_NODES 5000 
#define MAX_EDGES 10
#define MAX_REPLACEMENTS 1000

struct node {
    int head;
    struct node* edges[MAX_EDGES]; 
};

typedef struct node* Node;

static int num_nodes = MAX_NODES;
static int num_edges = MAX_EDGES;
static int num_replacements = MAX_REPLACEMENTS;

Node allocate_n()
{

    Node n = (Node)malloc(sizeof(struct node));
    if (n == NULL)
    {
        printf("Allocation failed\n");
        exit(1);
    }
    n->head = 0;
    for(int i=0; i<MAX_EDGES; i++) n->edges[i] = NULL;
    return n;
}

void replace(Node nodes[])
{
    int i = rand() % num_nodes;
    Node old_node = nodes[i];
    if (!old_node) return;

    Node new_node = allocate_n();
    
    new_node->head = old_node->head;
    for (int j = 0; j < old_node->head; j++) {
        new_node->edges[j] = old_node->edges[j];
    }

    nodes[i] = new_node;

    free(old_node);
}

int main(int argc, char *argv[])
{
    if (argc >= 2) num_nodes = atoi(argv[1]);
    if (argc >= 3) num_replacements = atoi(argv[2]);

    Node *nodes = (Node*)malloc(sizeof(Node) * num_nodes);
    if (!nodes) return 1;

    printf("Allocating %d nodes...\n", num_nodes);
    for (int i = 0; i < num_nodes; i++) {
        nodes[i] = allocate_n();
    }

    printf("Performing %d replacements...\n", num_replacements);
    for (int i = 0; i < num_replacements; i++) {
        replace(nodes);
    }

    printf("Cleaning up...\n");
    for (int i = 0; i < num_nodes; i++) {
        if (nodes[i]) free(nodes[i]);
    }
    free(nodes);

    printMemoryStats();
    return 0;
}