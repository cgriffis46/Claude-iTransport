#pragma once
#include <cstdint>

// CIP's own elementary data type identification codes — ODVA Volume
// 1, Appendix C, Table C-6.1, verified directly against the actual
// table rather than assumed from general knowledge. Used here so a
// registered tag carries a real, standard CIP type code from the
// start, rather than an invented one that would need translating
// later when a CIP wire-protocol layer is built on top of this
// registry.
enum class PlcDataType : uint8_t {
    Bool   = 0xC1,
    Sint   = 0xC2, // signed 8-bit
    Int    = 0xC3, // signed 16-bit
    Dint   = 0xC4, // signed 32-bit
    Lint   = 0xC5, // signed 64-bit
    Usint  = 0xC6, // unsigned 8-bit
    Uint   = 0xC7, // unsigned 16-bit
    Udint  = 0xC8, // unsigned 32-bit
    Ulint  = 0xC9, // unsigned 64-bit
    Real   = 0xCA, // 32-bit IEEE float
    Lreal  = 0xCB, // 64-bit IEEE float
    // A2hex in the real spec denotes a formal/abbreviated structure
    // type specification, not a single elementary type — reused here
    // as a catch-all marker for "opaque struct, contents not
    // individually typed by this registry," which is the honest
    // scope: full formal structure-member type dictionaries (Table
    // C-6.2 in the real spec) are real CIP functionality this class
    // does not attempt to reproduce.
    Struct = 0xA2,
};

// Maps a C++ type to its corresponding CIP elementary data type code
// at compile time — lets tag registration infer the correct type
// automatically (registering a float tag knows it's a Real without
// the caller ever stating so, and can't accidentally mislabel it),
// rather than requiring the type code to be passed, and potentially
// gotten wrong, by hand. Deliberately has NO default/generic
// definition: only the specializations below exist, so attempting to
// register an unsupported type (a raw struct, for instance) is a
// compile error here — not a silently-wrong runtime type code. Use
// PlcTagRegistry::registerStructTag() for anything without a direct
// elementary-type match.
template <typename T> struct PlcTypeTraits;

template <> struct PlcTypeTraits<bool>     { static constexpr PlcDataType kType = PlcDataType::Bool;  };
template <> struct PlcTypeTraits<int8_t>   { static constexpr PlcDataType kType = PlcDataType::Sint;  };
template <> struct PlcTypeTraits<int16_t>  { static constexpr PlcDataType kType = PlcDataType::Int;   };
template <> struct PlcTypeTraits<int32_t>  { static constexpr PlcDataType kType = PlcDataType::Dint;  };
template <> struct PlcTypeTraits<int64_t>  { static constexpr PlcDataType kType = PlcDataType::Lint;  };
template <> struct PlcTypeTraits<uint8_t>  { static constexpr PlcDataType kType = PlcDataType::Usint; };
template <> struct PlcTypeTraits<uint16_t> { static constexpr PlcDataType kType = PlcDataType::Uint;  };
template <> struct PlcTypeTraits<uint32_t> { static constexpr PlcDataType kType = PlcDataType::Udint; };
template <> struct PlcTypeTraits<uint64_t> { static constexpr PlcDataType kType = PlcDataType::Ulint; };
template <> struct PlcTypeTraits<float>    { static constexpr PlcDataType kType = PlcDataType::Real;  };
template <> struct PlcTypeTraits<double>   { static constexpr PlcDataType kType = PlcDataType::Lreal; };
