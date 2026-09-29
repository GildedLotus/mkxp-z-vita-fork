// SPDX-License-Identifier: GPL-3.0-only
// Derived from SceShaccCgExt (https://github.com/bythos14/SceShaccCgExt, GPL-3.0), src/shacccg_ext_oop.cpp at
// the pinned commit in vita/scripts/vitagl-pins.json. The red-black tree containers below are written for
// this repository; the rest is the upstream file. build-vitagl.sh copies this file over the pinned one.
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include <psp2/kernel/clib.h>

extern "C"
{
    // These are defined in shacccg_ext.c
    void *_sceShaccCgHeapAlloc(size_t size);
    void _sceShaccCgHeapFree(void *ptr);
    extern void (*_AtomicIncrement)(uint32_t *);
    extern uint32_t (*_AtomicDecrement)(uint32_t *);
}

/**
 * The set and map below are laid out exactly like the compiler's own containers (the pragma handlers
 * receive compiler-built ones), so the node and header layouts are fixed: left, parent, right, key
 * [, value], color, nil. The header (rootParent) doubles as the nil leaf, root->parent == header,
 * an empty tree links all three header pointers to itself, and header->left/right are the first and
 * last node. The balancing is the textbook red-black algorithm (Cormen et al., Introduction to
 * Algorithms, ch. 13), written for this repository.
 */

namespace // Unnamed namespace
{
    enum Color : std::uint8_t
    {
        RED,
        BLACK
    };

    class Allocatable
    {
    public:
        Allocatable() = default;
        void *operator new(std::size_t size)
        {
            return _sceShaccCgHeapAlloc(size);
        }

        void operator delete(void *ptr)
        {
            _sceShaccCgHeapFree(ptr);
        }
    };

    // link == nullptr builds the header, otherwise a leaf-linked red node.
    template <typename KeyType>
    struct SetNode : public Allocatable
    {
        typedef KeyType key_type;

        SetNode(SetNode *link, bool header) : left(link ? link : this), parent(left), right(left), key(), color(header ? BLACK : RED), nil(header) {}

        SetNode *left;
        SetNode *parent;
        SetNode *right;
        KeyType key;
        Color color;
        bool nil;
    };

    template <typename KeyType, typename ValueType>
    struct MapNode : public Allocatable
    {
        typedef KeyType key_type;

        MapNode(MapNode *link, bool header) : left(link ? link : this), parent(left), right(left), key(), value(), color(header ? BLACK : RED), nil(header) {}

        MapNode *left;
        MapNode *parent;
        MapNode *right;
        KeyType key;
        ValueType value;
        Color color;
        bool nil;
    };

    template <typename Node>
    class RedBlackTree : public Allocatable
    {
    public:
        typedef typename Node::key_type Key;

        std::uint32_t unk_0x0;
        Node *rootParent;   // Header node; also the nil leaf.
        std::uint32_t size; // Number of nodes.

        RedBlackTree() : unk_0x0(0), rootParent(new Node(nullptr, true)), size(0) {}
        RedBlackTree(const RedBlackTree &) = delete;
        RedBlackTree &operator=(const RedBlackTree &) = delete;
        ~RedBlackTree()
        {
            destroy(rootParent->parent);
            delete rootParent;
        }

        Node *search(const Key &key) const
        {
            Node *node = rootParent->parent;
            while (node != rootParent)
            {
                if (key < node->key)
                    node = node->left;
                else if (node->key < key)
                    node = node->right;
                else
                    return node;
            }
            return nullptr;
        }

        // Returns the node holding key, adding it (value-initialised) when absent.
        Node *insert(const Key &key)
        {
            Node *parent = rootParent;
            Node *node = rootParent->parent;
            bool toLeft = false;

            while (node != rootParent)
            {
                parent = node;
                if (key < node->key)
                {
                    node = node->left;
                    toLeft = true;
                }
                else if (node->key < key)
                {
                    node = node->right;
                    toLeft = false;
                }
                else
                {
                    return node;
                }
            }

            Node *added = new Node(rootParent, false);
            added->key = key;
            added->parent = parent;
            if (parent == rootParent)
            {
                rootParent->parent = rootParent->left = rootParent->right = added;
            }
            else if (toLeft)
            {
                parent->left = added;
                if (parent == rootParent->left)
                    rootParent->left = added;
            }
            else
            {
                parent->right = added;
                if (parent == rootParent->right)
                    rootParent->right = added;
            }

            rebalanceAfterInsert(added);
            size++;
            return added;
        }

        bool remove(const Key &key)
        {
            Node *node = search(key);
            if (node == nullptr)
                return false;

            erase(node);
            size--;
            return true;
        }

    private:
        typedef Node *Node::*Link;

        static Link opposite(Link side)
        {
            return side == &Node::left ? &Node::right : &Node::left;
        }

        bool isBlack(const Node *node) const
        {
            return node == rootParent || node->color == BLACK;
        }

        // The child of x at `up` takes x's place and x becomes that child's `down` child.
        void rotate(Node *x, Link up, Link down)
        {
            Node *y = x->*up;

            x->*up = y->*down;
            if (y->*down != rootParent)
                (y->*down)->parent = x;
            y->parent = x->parent;
            if (x->parent == rootParent)
                rootParent->parent = y;
            else if (x == x->parent->left)
                x->parent->left = y;
            else
                x->parent->right = y;
            y->*down = x;
            x->parent = y;
        }

        // Puts subtree `to` where `from` hangs; `to` may be the header.
        void replace(Node *from, Node *to)
        {
            if (from->parent == rootParent)
                rootParent->parent = to;
            else if (from == from->parent->left)
                from->parent->left = to;
            else
                from->parent->right = to;
            if (to != rootParent)
                to->parent = from->parent;
        }

        Node *extreme(Node *node, Link side) const
        {
            while (node->*side != rootParent)
                node = node->*side;
            return node;
        }

        void rebalanceAfterInsert(Node *node)
        {
            while (node->parent != rootParent && node->parent->color == RED)
            {
                Node *parent = node->parent;
                Node *grand = parent->parent; // A red parent is never the root.
                Link near = (parent == grand->left) ? &Node::left : &Node::right;
                Link far = opposite(near);
                Node *uncle = grand->*far;

                if (uncle != rootParent && uncle->color == RED)
                {
                    parent->color = BLACK;
                    uncle->color = BLACK;
                    grand->color = RED;
                    node = grand;
                    continue;
                }
                if (node == parent->*far)
                {
                    rotate(parent, far, near);
                    node = parent;
                    parent = node->parent;
                }
                parent->color = BLACK;
                grand->color = RED;
                rotate(grand, near, far);
            }
            rootParent->parent->color = BLACK;
        }

        void erase(Node *node)
        {
            if (rootParent->left == node)
                rootParent->left = (node->right != rootParent) ? extreme(node->right, &Node::left) : node->parent;
            if (rootParent->right == node)
                rootParent->right = (node->left != rootParent) ? extreme(node->left, &Node::right) : node->parent;

            Node *moved = node; // The node whose colour leaves its place in the tree.
            Node *child;
            Node *childParent; // Kept apart from child->parent, because child may be the header.
            Color removed = moved->color;

            if (node->left == rootParent)
            {
                child = node->right;
                childParent = node->parent;
                replace(node, child);
            }
            else if (node->right == rootParent)
            {
                child = node->left;
                childParent = node->parent;
                replace(node, child);
            }
            else
            {
                moved = extreme(node->right, &Node::left);
                removed = moved->color;
                child = moved->right;
                if (moved->parent == node)
                {
                    childParent = moved;
                }
                else
                {
                    childParent = moved->parent;
                    replace(moved, child);
                    moved->right = node->right;
                    moved->right->parent = moved;
                }
                replace(node, moved);
                moved->left = node->left;
                moved->left->parent = moved;
                moved->color = node->color;
            }

            if (removed == BLACK)
                rebalanceAfterErase(child, childParent);
            delete node;
        }

        void rebalanceAfterErase(Node *node, Node *parent)
        {
            while (node != rootParent->parent && isBlack(node))
            {
                Link near = (node == parent->left) ? &Node::left : &Node::right;
                Link far = opposite(near);
                Node *sibling = parent->*far;

                if (sibling->color == RED)
                {
                    sibling->color = BLACK;
                    parent->color = RED;
                    rotate(parent, far, near);
                    sibling = parent->*far;
                }
                if (isBlack(sibling->left) && isBlack(sibling->right))
                {
                    sibling->color = RED;
                    node = parent;
                    parent = node->parent;
                    continue;
                }
                if (isBlack(sibling->*far))
                {
                    (sibling->*near)->color = BLACK;
                    sibling->color = RED;
                    rotate(sibling, near, far);
                    sibling = parent->*far;
                }
                sibling->color = parent->color;
                parent->color = BLACK;
                (sibling->*far)->color = BLACK;
                rotate(parent, far, near);
                node = rootParent->parent;
                break;
            }
            if (node != rootParent)
                node->color = BLACK;
        }

        void destroy(Node *node)
        {
            if (node == rootParent)
                return;
            destroy(node->left);
            destroy(node->right);
            delete node;
        }
    };

    template <typename KeyType>
    class set : public RedBlackTree<SetNode<KeyType> >
    {
    public:
        typedef SetNode<KeyType> node;

        set() {}
        set(set<KeyType> &s)
        {
            this->unk_0x0 = s.unk_0x0;
            copySubtree(s.rootParent->parent, s.rootParent);
        }

    private:
        void copySubtree(const node *from, const node *nil)
        {
            if (from == nil)
                return;
            this->insert(from->key);
            copySubtree(from->left, nil);
            copySubtree(from->right, nil);
        }
    };

    template <typename KeyType, typename ValueType>
    class map : public RedBlackTree<MapNode<KeyType, ValueType> >
    {
    public:
        typedef MapNode<KeyType, ValueType> node;
    };

    template <typename ValueType>
    class vector : public Allocatable
    {
    public:
        vector() = default;
        ~vector() {}

        ValueType *begin;
        ValueType *end;
        ValueType *storageEnd;

        ValueType &back()
        {
            return *(end - 1);
        }

        void push_back(ValueType &val)
        {
            if (end == storageEnd)
            {
                size_t newSize = ((storageEnd - begin) + 8) * sizeof(ValueType);
                ValueType *newBegin = reinterpret_cast<ValueType *>(_sceShaccCgHeapAlloc(newSize));
                sceClibMemcpy(newBegin, begin, end - begin);
                storageEnd = reinterpret_cast<ValueType *>(newBegin + newSize / sizeof(ValueType));
                end = reinterpret_cast<ValueType *>(newBegin + (end - begin));
                begin = reinterpret_cast<ValueType *>(newBegin);
            }

            end++;
            back() = val;
        }

        void pop_back()
        {
            end--;
        }

        size_t size()
        {
            return (end - begin) / sizeof(ValueType);
        }
    };

    struct BufferInfo
    {
        uint32_t storage;
        uint32_t readwrite;
        uint32_t strip;
        uint32_t symbols;

        BufferInfo() : storage(0), readwrite(0), strip(0), symbols(0) {}
    };

    // The 32-bit layout of the compiler's own containers, which the pragma handlers share with it.
    typedef set<std::uint32_t> U32Set;
    typedef map<std::uint32_t, BufferInfo> U32BufferMap;
    static_assert(sizeof(void *) != 4 ||
                      (sizeof(U32Set) == 12 && offsetof(U32Set, rootParent) == 4 && offsetof(U32Set, size) == 8 &&
                       sizeof(U32Set::node) == 20 && offsetof(U32Set::node, key) == 12 && offsetof(U32Set::node, color) == 16 &&
                       sizeof(U32BufferMap::node) == 36 && offsetof(U32BufferMap::node, value) == 16 && offsetof(U32BufferMap::node, color) == 32),
                  "container layout");

    class WarningContainer : public Allocatable
    {
    public:
        WarningContainer() = default;
        WarningContainer(WarningContainer &container) : refCount(0), reset(false), disabledWarnings(container.disabledWarnings), elevatedWarnings(container.elevatedWarnings)  {}

        uint32_t refCount;
        bool reset;
        set<uint32_t> disabledWarnings;
        set<uint32_t> elevatedWarnings;

        WarningContainer *retain()
        {
            _AtomicIncrement(&refCount);
            return this;
        }

        void release()
        {
            if (_AtomicDecrement(&refCount) == 0)
                delete this;
        }
    };
}

extern "C" void ProcessPragma_Warning_PushWarningState(uint8_t *object)
{
    vector<WarningContainer *> *vec = reinterpret_cast<vector<WarningContainer *> *>(object + 0x18);
    WarningContainer *oldContainer = *reinterpret_cast<WarningContainer **>(*reinterpret_cast<uint8_t **>(object + 0x10) + 0x60);
    WarningContainer *newContainer = new WarningContainer(*oldContainer);

    vec->push_back(newContainer);
    newContainer->retain();

    *reinterpret_cast<WarningContainer **>(*reinterpret_cast<uint8_t **>(object + 0x10) + 0x60) = newContainer;
    oldContainer->release();
    newContainer->retain();
}

extern "C" bool ProcessPragma_Warning_PopWarningState(uint8_t *object)
{
    vector<WarningContainer *> *vec = reinterpret_cast<vector<WarningContainer *> *>(object + 0x18);
    WarningContainer *oldContainer = *reinterpret_cast<WarningContainer **>(*reinterpret_cast<uint8_t **>(object + 0x10) + 0x60);
    WarningContainer *newContainer;

    if (vec->size() == 1)
    {
        return false;
    }

    vec->back() = nullptr;
    vec->pop_back();
    oldContainer->release();

    newContainer = vec->back();
    *reinterpret_cast<WarningContainer **>(*reinterpret_cast<uint8_t **>(object + 0x10) + 0x60) = newContainer;
    oldContainer->release();
    newContainer->retain();

    return true;
}

extern "C" void ProcessPragma_Warning_ResetWarningContainer(uint8_t *object)
{
    vector<WarningContainer *> *vec = reinterpret_cast<vector<WarningContainer *> *>(object + 0x18);
    WarningContainer *oldContainer = *reinterpret_cast<WarningContainer **>(*reinterpret_cast<uint8_t **>(object + 0x10) + 0x60);
    WarningContainer *newContainer;

    if (oldContainer->reset)
    {
        newContainer = new WarningContainer(*oldContainer);

        *reinterpret_cast<WarningContainer **>(*reinterpret_cast<uint8_t **>(object + 0x10) + 0x60) = newContainer;
        oldContainer->release();
        newContainer->retain();

        vec->back() = newContainer;
        oldContainer->release();
        newContainer->retain();
    }
}

extern "C" void ProcessPragma_Warning_RemoveDiagnostic(void *p, uint32_t key)
{
    set<uint32_t> *diagnosticSet = reinterpret_cast<set<uint32_t> *>(p);

    diagnosticSet->remove(key);
}

extern "C" BufferInfo *ProcessPragma_Buffer_GetBufferInfo(void *p, uint32_t key)
{
    map<uint32_t, BufferInfo>::node *bufferNode;
    map<uint32_t, BufferInfo> *bufferMap = reinterpret_cast<map<uint32_t, BufferInfo> *>(p);

    bufferNode = bufferMap->search(key);
    if (bufferNode != nullptr)
        return &bufferNode->value;

    bufferNode = bufferMap->insert(key);

    return &bufferNode->value;
}