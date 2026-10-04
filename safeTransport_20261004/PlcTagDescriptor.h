#pragma once
#include <cstddef>
#include <string>
#include "PlcDataType.h"

// Type-erased description of one registered tag: where its actual
// bytes live, how big it is, what CIP elementary type it represents,
// and whether external writes are allowed. The registry stores these,
// not the original typed reference — erasing the type is what lets
// one registry hold tags of many different C++ types uniformly.
struct PlcTagDescriptor {
    std::string name;
    void*       data;       // points directly at the live variable/struct — NOT a copy
    size_t      sizeBytes;
    PlcDataType dataType;
    bool        writable;
};
