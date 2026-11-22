#include "adjust/jni_lookup.hpp"

#include "minecraft/mappings.hpp"

namespace slinky::adjust::jni {

namespace {

bool usable(events::JniContext* jni) noexcept {
    return jni != nullptr && jni->env != nullptr && jni->mappings != nullptr;
}

const char* pick_desc(events::JniContext* jni, const char* vanilla_desc, const char* named_desc) noexcept {
    return (jni->mappings->mapping_flavor == 0) ? vanilla_desc : named_desc;
}

} // namespace

bool is_1_8(events::JniContext* jni) noexcept {
    return usable(jni) && jni->mappings->version == 1;
}

const char* pick(events::JniContext* jni, const Names& names) noexcept {
    if (!usable(jni)) {
        return nullptr;
    }
    switch (jni->mappings->mapping_flavor) {
        case 0: return names.vanilla;
        case 1: return names.mcp;
        default: return names.srg;
    }
}

bool clear_exception(events::JniContext* jni) noexcept {
    if (!usable(jni) || !jni->env->ExceptionCheck()) {
        return false;
    }
    jni->env->ExceptionClear();
    return true;
}

jclass find_class(events::JniContext* jni, const char* vanilla_name, const char* mcp_name) noexcept {
    if (!is_1_8(jni)) {
        return nullptr;
    }
    const char* name = (jni->mappings->mapping_flavor == 0) ? vanilla_name : mcp_name;
    if (name == nullptr) {
        return nullptr;
    }
    jclass local = minecraft::load_class(&jni->mappings->client_class_loader, jni->env, name);
    if (local == nullptr) {
        clear_exception(jni);
        return nullptr;
    }
    auto global = static_cast<jclass>(jni->env->NewGlobalRef(local));
    jni->env->DeleteLocalRef(local);
    return global;
}

jfieldID field(events::JniContext* jni, jclass cls, const Names& names,
               const char* vanilla_desc, const char* named_desc) noexcept {
    if (!is_1_8(jni) || cls == nullptr) {
        return nullptr;
    }
    const char* name = pick(jni, names);
    if (name == nullptr) {
        return nullptr;
    }
    jfieldID id = jni->env->GetFieldID(cls, name, pick_desc(jni, vanilla_desc, named_desc));
    if (id == nullptr) {
        clear_exception(jni);
    }
    return id;
}

jfieldID static_field(events::JniContext* jni, jclass cls, const Names& names,
                      const char* vanilla_desc, const char* named_desc) noexcept {
    if (!is_1_8(jni) || cls == nullptr) {
        return nullptr;
    }
    const char* name = pick(jni, names);
    if (name == nullptr) {
        return nullptr;
    }
    jfieldID id = jni->env->GetStaticFieldID(cls, name, pick_desc(jni, vanilla_desc, named_desc));
    if (id == nullptr) {
        clear_exception(jni);
    }
    return id;
}

jmethodID method(events::JniContext* jni, jclass cls, const Names& names,
                 const char* vanilla_desc, const char* named_desc) noexcept {
    if (!is_1_8(jni) || cls == nullptr) {
        return nullptr;
    }
    const char* name = pick(jni, names);
    if (name == nullptr) {
        return nullptr;
    }
    jmethodID id = jni->env->GetMethodID(cls, name, pick_desc(jni, vanilla_desc, named_desc));
    if (id == nullptr) {
        clear_exception(jni);
    }
    return id;
}

jmethodID static_method(events::JniContext* jni, jclass cls, const Names& names,
                        const char* vanilla_desc, const char* named_desc) noexcept {
    if (!is_1_8(jni) || cls == nullptr) {
        return nullptr;
    }
    const char* name = pick(jni, names);
    if (name == nullptr) {
        return nullptr;
    }
    jmethodID id = jni->env->GetStaticMethodID(cls, name, pick_desc(jni, vanilla_desc, named_desc));
    if (id == nullptr) {
        clear_exception(jni);
    }
    return id;
}

} // namespace slinky::adjust::jni
