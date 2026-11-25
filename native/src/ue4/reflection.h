// Minimal, read-only access to Unreal Engine 4.27 reflection data
// (FNamePool / GUObjectArray / UStruct / FProperty) inside the host process.
//
// Nothing here is SCUM-specific and no addresses are hard-coded: the global
// tables are found by scanning the executable's data section for structures
// that look like them, and struct member offsets are discovered at runtime
// from known engine objects. See docs/engine-notes/reflection.md.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace overseer::ue4 {

struct FName {
    int32_t comparisonIndex = 0;
    int32_t number = 0;
};

// Discovered layout of engine structs (byte offsets).
struct Layout {
    // UObject (stable since UE4.0)
    uint32_t objectIndex = 0x0C;
    uint32_t objectClass = 0x10;
    uint32_t objectName = 0x18;
    uint32_t objectOuter = 0x20;
    // UField
    uint32_t fieldNext = 0x28;
    // UStruct (discovered)
    uint32_t structSuper = 0;
    uint32_t structChildren = 0;
    uint32_t structChildProperties = 0;
    uint32_t structPropertiesSize = 0;
    // FField / FProperty (UE4.25+)
    uint32_t ffieldClass = 0x08;
    uint32_t ffieldNext = 0x20;
    uint32_t ffieldName = 0x28;
    uint32_t propertyArrayDim = 0x38;
    uint32_t propertyElementSize = 0x3C;
    uint32_t propertyFlags = 0x40;
    uint32_t propertyOffset = 0x4C;
    // UFunction (discovered)
    uint32_t functionFlags = 0;
    uint32_t functionParmsSize = 0;
    uint32_t functionNative = 0;
    // UClass::Interfaces (TArray<FImplementedInterface>), discovered; 0 if unknown
    uint32_t classInterfaces = 0;
    // UObject::ProcessEvent vtable index (validated)
    uint32_t processEventIndex = 0;
};

struct PropertyInfo {
    std::string name;
    std::string type;  // FFieldClass name, e.g. "StrProperty"
    int32_t offset = 0;
    int32_t elementSize = 0;
    int32_t arrayDim = 0;
    uint64_t flags = 0;
};

class Reflection {
public:
    // Locates FNamePool and GUObjectArray and discovers struct offsets.
    // Safe to retry until the engine has finished initialising.
    bool Initialize(std::string& error);
    bool Ready() const { return ready_; }

    const Layout& GetLayout() const { return layout_; }
    uintptr_t NameBlocks() const { return nameBlocks_; }
    uintptr_t ObjectArray() const { return objectArray_; }

    std::string NameToString(FName name) const;

    int32_t ObjectCount() const;
    uintptr_t ObjectAt(int32_t index) const;

    int32_t ParmsSize(uintptr_t ufunction) const;
    // Offset of a named parameter/property on a struct or function, or -1.
    int32_t PropertyOffset(uintptr_t ustruct, const std::string& name) const;

    std::string ObjectName(uintptr_t object) const;
    uintptr_t ObjectClass(uintptr_t object) const;
    uintptr_t ObjectOuter(uintptr_t object) const;
    std::string ClassName(uintptr_t object) const { return ObjectName(ObjectClass(object)); }
    // "Package.Outer.Name"
    std::string PathName(uintptr_t object) const;
    // "ClassName Package.Outer.Name"
    std::string FullName(uintptr_t object) const;

    uintptr_t SuperStruct(uintptr_t ustruct) const;
    bool IsA(uintptr_t object, uintptr_t ofClass) const;

    // First object whose full name matches exactly, e.g. "Class /Script/ConZ.ConZPlayerController".
    uintptr_t FindObject(const std::string& fullName) const;
    void ForEachObject(const std::function<void(int32_t index, uintptr_t object)>& fn) const;

    // UFunctions declared directly on a class (not inherited).
    std::vector<uintptr_t> Functions(uintptr_t ustruct) const;
    // Properties declared directly on a struct/function (not inherited).
    std::vector<PropertyInfo> Properties(uintptr_t ustruct) const;
    int32_t PropertiesSize(uintptr_t ustruct) const;
    uint32_t FunctionFlags(uintptr_t ufunction) const;
    uintptr_t FunctionNative(uintptr_t ufunction) const;

    // Discovers UClass::Interfaces offset by voting across sampled classes.
    // Best-effort and non-fatal; returns false if it could not be found.
    bool DiscoverInterfaceOffset();

    // UClasses that implement the given UInterface class (via their
    // FImplementedInterface array). Requires DiscoverInterfaceOffset() first.
    std::vector<uintptr_t> Implementers(uintptr_t interfaceClass) const;

    // First non-CDO (name not starting "Default__") live instance of a class
    // or its subclasses, or 0.
    uintptr_t FindLiveInstance(uintptr_t ofClass) const;

    // Writes everything we have learned plus every object matching `filter`
    // (functions with signatures, classes with properties) to `path`.
    bool Dump(const std::wstring& path, const std::function<bool(const std::string&)>& filter,
              std::string& error) const;

private:
    bool FindNamePool(std::string& error);
    bool FindObjectArray(std::string& error);
    bool DiscoverStructLayout(std::string& error);
    bool DiscoverFunctionLayout(std::string& error);
    bool DiscoverProcessEvent(std::string& error);

    bool ready_ = false;
    Layout layout_;
    uintptr_t nameBlocks_ = 0;   // &FNamePool::Entries.Blocks[0]
    uintptr_t objectArray_ = 0;  // &GUObjectArray.ObjObjects (FChunkedFixedUObjectArray)
    uintptr_t textBegin_ = 0, textEnd_ = 0;
};

}  // namespace overseer::ue4
