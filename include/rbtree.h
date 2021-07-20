#ifndef _RBTREE_H_
#define _RBTREE_H_

#include <stdint.h>
#include <stddef.h>

typedef struct rbtree_node_s rbtree_node_t;
struct rbtree_node_s{
    uint64_t key;
    struct rbtree_node_s *left;
    struct rbtree_node_s *right;
    struct rbtree_node_s *parent;
    uint8_t color;
};

void rbtree_node_init(rbtree_node_t *node);

typedef struct rbtree_s rbtree_t;
struct rbtree_s{
    rbtree_node_t *root;
    rbtree_node_t sentinel;
};

void rbtree_init(rbtree_t *tree);
void rbtree_destory(rbtree_t *tree);
void rbtree_insert(rbtree_t *tree,rbtree_node_t* node);
void rbtree_delete(rbtree_t *tree,rbtree_node_t* node);
rbtree_node_t* rbtree_search(rbtree_t *tree,uint64_t key);
rbtree_node_t* rbtree_min(rbtree_t *tree,rbtree_node_t* node);
rbtree_node_t* rbtree_next(rbtree_t *tree,rbtree_node_t* node);

#define rbtree_data(node, type, link) \
    (type *) ((u_char *) node - offsetof(type, link))

#endif
