#include <stddef.h> //for NULL
#include "rbtree.h"

#define rbtree_black 0
#define rbtree_red   1

void rbtree_node_init(rbtree_node_t *node){
    if(node == NULL)
        return;

    node->left = NULL;
    node->right = NULL;
    node->parent = NULL;

    return;
}

void rbtree_init(rbtree_t *tree){
    if(tree == NULL)
        return;

    tree->sentinel.left = &tree->sentinel;
    tree->sentinel.right = &tree->sentinel;
    tree->sentinel.parent = &tree->sentinel;
    tree->sentinel.color = rbtree_black;

    tree->root = &tree->sentinel;

    return;
}

void rbtree_destory(rbtree_t *tree){
    if(tree == NULL)
        return;

    tree->root = &tree->sentinel;
}

static void rbtree_left_rotate(rbtree_t *tree,rbtree_node_t* x){
    if(x == &tree->sentinel){
        return;
    }

    rbtree_node_t*y = x->right;
    if(y == &tree->sentinel){
        return;
    }

    x->right = y->left;
    if(y->left != &tree->sentinel){
        y->left->parent = x;
    }

    y->parent = x->parent;
    if(x == tree->root){
        tree->root = y;
    }else if(x == x->parent->left){
        x->parent->left = y;
    }else{
        x->parent->right = y;
    }

    x->parent = y;
    y->left = x;

    return;
}

static void rbtree_right_rotate(rbtree_t *tree,rbtree_node_t* x){
    if(x == &tree->sentinel){
        return;
    }

    rbtree_node_t*y = x->left;
    if(y == &tree->sentinel){
        return;
    }

    x->left = y->right;
    if(y->right != &tree->sentinel){
        y->right->parent = x;
    }

    y->parent = x->parent;
    if(x == tree->root){
        tree->root = y;
    }else if(x == x->parent->left){
        x->parent->left = y;
    }else{
        x->parent->right = y;
    }

    x->parent = y;
    y->right = x;

    return;
}

void rbtree_insert(rbtree_t *tree,rbtree_node_t* node){
    if(tree == NULL || node == NULL || node == &tree->sentinel)
        return;

    rbtree_node_t *x,*x_tmp;
    x = tree->root;
    x_tmp = &tree->sentinel;
    while(x != &tree->sentinel){
        if(node->key<x->key){
            x_tmp = x;
            x = x->left;
        }else{
            x_tmp = x;
            x = x->right;
        }
    }

    node->parent = x_tmp;
    node->left = &tree->sentinel;
    node->right = &tree->sentinel;
    node->color = rbtree_red;

    if(tree->root == &tree->sentinel){
        tree->root = node;
    }else if(node->key < x_tmp->key){
        x_tmp->left = node;
    }else{
        x_tmp->right = node;
    }

    while(node->parent->color == rbtree_red){

        if(node->parent == node->parent->parent->left){
            x = node->parent->parent->right;
            if(x->color == rbtree_red){
                node->parent->color = rbtree_black;
                x->color = rbtree_black;
                node->parent->parent->color = rbtree_red;
                node = node->parent->parent;
            }else{
                if(node == node->parent->right){
                    node = node->parent;
                    rbtree_left_rotate(tree,node);
                }

                node->parent->color = rbtree_black;
                node->parent->parent->color = rbtree_red;
                rbtree_right_rotate(tree,node->parent->parent);
            }
        }else{
            x = node->parent->parent->left;
            if(x->color == rbtree_red){
                node->parent->color = rbtree_black;
                x->color = rbtree_black;
                node->parent->parent->color = rbtree_red;
                node = node->parent->parent;
            }else{
                if(node == node->parent->left){
                    node = node->parent;
                    rbtree_right_rotate(tree,node);
                }

                node->parent->color = rbtree_black;
                node->parent->parent->color = rbtree_red;
                rbtree_left_rotate(tree,node->parent->parent);
            }
        }

    }
    tree->root->color = rbtree_black;
    return;
}

static rbtree_node_t* rbtree_min_node(rbtree_t *tree,rbtree_node_t* node){
    while(node->left != &tree->sentinel){
        node = node->left;
    }
    return node;
}

void rbtree_delete(rbtree_t *tree,rbtree_node_t* node){
    if(tree == NULL || node == NULL || node == &tree->sentinel){
        return;
    }

    if(node->left == NULL || node->right == NULL || node->parent == NULL){
        return;
    }


    rbtree_node_t *temp,*subst,*w;
    unsigned char color;

    if(node->left == &tree->sentinel){
        subst = node;
        temp = node->right;
    }else if(node->right == &tree->sentinel){
        subst = node;
        temp = node->left;
    }else{
        subst = rbtree_min_node(tree,node->right);
        temp = subst->right;
    }


    if(subst == tree->root){
        tree->root = temp;
        temp->color = rbtree_black;
        temp->parent = &tree->sentinel;
        
        node->left = NULL;
        node->right = NULL;
        node->parent = NULL;

        tree->sentinel.parent = &tree->sentinel;
        return;
    }

    color = subst->color;

    if(subst == subst->parent->left){
        subst->parent->left = temp;
    }else{
        subst->parent->right = temp;
    }

    if(subst == node){
        temp->parent = subst->parent;
    }else{
        if(subst->parent == node){
            temp->parent = subst;
        }else{
            temp->parent = subst->parent;
        }
        subst->left = node->left;
        subst->right = node->right;
        subst->parent = node->parent;
        subst->color = node->color;

        if(node == tree->root){
            tree->root = subst;
        }else{
            if(node == node->parent->left){
                node->parent->left = subst;
            }else{
                node->parent->right = subst;
            }
        }
        
        if(subst->left != &tree->sentinel){
            subst->left->parent = subst;
        }

        if(subst->right != &tree->sentinel){
            subst->right->parent = subst;
        }
    }

    node->left = NULL;
    node->right = NULL;
    node->parent = NULL;

    if(color == rbtree_red){
        tree->sentinel.parent = &tree->sentinel;
        return;
    }

    while(temp != tree->root && temp->color == rbtree_black){
        if(temp == temp->parent->left){
            w = temp->parent->right;
            if(w->color == rbtree_red){
                w->color = rbtree_black;
                temp->parent->color = rbtree_red;
                rbtree_left_rotate(tree,temp->parent);
                w = temp->parent->right;
            }

            if(w->left->color == rbtree_black && w->right->color == rbtree_black){
                w->color = rbtree_red;
                temp = temp->parent;
            }else{
                if(w->left->color == rbtree_red){
                    w->color = rbtree_red;
                    w->left->color = rbtree_black;
                    rbtree_right_rotate(tree,w);
                    w = temp->parent->right;
                }

                w->color = temp->parent->color;
                temp->parent->color = rbtree_black;
                w->right->color = rbtree_black;
                rbtree_left_rotate(tree,temp->parent);
                temp = tree->root;
            }
        }else{
            w = temp->parent->left;
            if(w->color == rbtree_red){
                w->color = rbtree_black;
                temp->parent->color = rbtree_red;
                rbtree_right_rotate(tree,temp->parent);
                w = temp->parent->left;
            }

            if(w->left->color == rbtree_black && w->right->color == rbtree_black){
                w->color = rbtree_red;
                temp = temp->parent;
            }else{
                if(w->right->color == rbtree_red){
                    w->color = rbtree_red;
                    w->right->color = rbtree_black;
                    rbtree_left_rotate(tree,w);
                    w = temp->parent->left;
                }

                w->color = temp->parent->color;
                temp->parent->color = rbtree_black;
                w->left->color = rbtree_black;
                rbtree_right_rotate(tree,temp->parent);
                temp = tree->root;
            }
        }
    }
    
    temp->color = rbtree_black;

    tree->sentinel.parent = &tree->sentinel;

    return;
}

rbtree_node_t* rbtree_search(rbtree_t *tree,uint64_t key){
    if(tree == NULL)
        return NULL;

    rbtree_node_t *node = tree->root;
    while(node != &tree->sentinel){
        if(key == node->key){
            break;
        }else if(key < node->key){
            node = node->left;
        }else{
            node = node->right;
        }
    }

    if(node == &tree->sentinel){
        node = NULL;
    }
    
    return node;
}

rbtree_node_t* rbtree_min(rbtree_t *tree,rbtree_node_t* node){
    if(tree == NULL || node == NULL || node==&tree->sentinel){
        return NULL;
    }

    if(node->left == NULL || node->right == NULL || node->parent == NULL){
        return NULL;
    }

    while(node->left != &tree->sentinel){
        node = node->left;
    }
    if(node == &tree->sentinel){
        node = NULL;
    }
    return node;
}

rbtree_node_t* rbtree_next(rbtree_t *tree,rbtree_node_t* node){
    if(node == NULL || node == NULL || node == &tree->sentinel){
        return NULL;
    }

    if(node->left == NULL || node->right == NULL || node->parent == NULL){
        return NULL;
    }

    if(node->right != &tree->sentinel){
        return rbtree_min(tree,node->right);
    }


    for(;;){
        if(node == tree->root){
            return NULL;
            //return tree->sentinel;
        }

        if(node == node->parent->left){
            return node->parent;
        }
        node = node->parent;
    }
}