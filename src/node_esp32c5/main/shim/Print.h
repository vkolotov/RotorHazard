// CircularBuffer.h only names Print in its debug helpers, which the node
// never calls.
#ifndef RHNODE_C5_PRINT_H
#define RHNODE_C5_PRINT_H
class Print {
public:
    virtual ~Print() {}
};
#endif
