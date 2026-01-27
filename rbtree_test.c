#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "memory.h"

enum Color { RED, BLACK };

struct RBNode {
    int data;
    enum Color color;
    struct RBNode *left, *right, *parent;
};

typedef struct RBNode* Node;

Node create_node(int data) {
    Node n = (Node)__wrap_malloc(sizeof(struct RBNode));
    if (n == NULL) {
        printf("Allocation failed!\n");
        exit(1);
    }
    n->data = data;
    n->color = RED;
    n->left = n->right = n->parent = NULL;
    return n;
}

void rotate_left(Node *root, Node x) {
    Node y = x->right;
    x->right = y->left;
    if (y->left != NULL) y->left->parent = x;
    y->parent = x->parent;
    if (x->parent == NULL) *root = y;
    else if (x == x->parent->left) x->parent->left = y;
    else x->parent->right = y;
    y->left = x;
    x->parent = y;
}

void rotate_right(Node *root, Node y) {
    Node x = y->left;
    y->left = x->right;
    if (x->right != NULL) x->right->parent = y;
    x->parent = y->parent;
    if (y->parent == NULL) *root = x;
    else if (y == y->parent->left) y->parent->left = x;
    else y->parent->right = x;
    x->right = y;
    y->parent = x;
}

void fix_insert(Node *root, Node z) {
    while (z != *root && z->parent->color == RED) {
        if (z->parent == z->parent->parent->left) {
            Node y = z->parent->parent->right;
            if (y != NULL && y->color == RED) {
                z->parent->color = BLACK;
                y->color = BLACK;
                z->parent->parent->color = RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->right) {
                    z = z->parent;
                    rotate_left(root, z);
                }
                z->parent->color = BLACK;
                z->parent->parent->color = RED;
                rotate_right(root, z->parent->parent);
            }
        } else {
            Node y = z->parent->parent->left;
            if (y != NULL && y->color == RED) {
                z->parent->color = BLACK;
                y->color = BLACK;
                z->parent->parent->color = RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->left) {
                    z = z->parent;
                    rotate_right(root, z);
                }
                z->parent->color = BLACK;
                z->parent->parent->color = RED;
                rotate_left(root, z->parent->parent);
            }
        }
    }
    (*root)->color = BLACK;
}

void insert(Node *root, int data) {
    Node z = create_node(data);
    Node y = NULL;
    Node x = *root;

    while (x != NULL) {
        y = x;
        if (z->data < x->data) x = x->left;
        else x = x->right;
    }

    z->parent = y;
    if (y == NULL) *root = z;
    else if (z->data < y->data) y->left = z;
    else y->right = z;

    fix_insert(root, z);
}

void delete_tree(Node root) {
    if (root == NULL) return;
    delete_tree(root->left);
    delete_tree(root->right);
    __wrap_free(root);
}

int main(int argc, char *argv[]) {
    int num_ops = 10000;
    if (argc >= 2) num_ops = atoi(argv[1]);

    Node root = NULL;

    printf("Starting RB Tree Test with %d insertions...\n", num_ops);
    for (int i = 0; i < num_ops; i++) {
        insert(&root, rand() % num_ops);
    }

    printMemoryStats();

    printf("Deleting tree to test physical page reclamation...\n");
    delete_tree(root);
    root = NULL;

    printf("Final Statistics:\n");
    printMemoryStats();

    return 0;
}
