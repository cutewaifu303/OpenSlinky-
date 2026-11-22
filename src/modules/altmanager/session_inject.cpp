#include "modules/altmanager/session_inject.hpp"

#include <string>

#include "adjust/jni_lookup.hpp"
#include "minecraft/mappings.hpp"

namespace slinky::altmanager {

namespace {

std::string strip_dashes(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c != '-') {
            out.push_back(c);
        }
    }
    return out;
}

} // namespace

bool inject_session(events::JniContext* jni, jobject minecraft, const Account& account) noexcept {
    if (jni == nullptr || jni->env == nullptr || minecraft == nullptr || !adjust::jni::is_1_8(jni)) {
        return false;
    }
    JNIEnv* env = jni->env;
    const jclass mc_cls = (jni->mappings != nullptr) ? jni->mappings->cls_minecraft : nullptr;
    if (mc_cls == nullptr) {
        return false;
    }

    static const jclass session_cls =
        adjust::jni::find_class(jni, "avm", "net/minecraft/util/Session");
    if (session_cls == nullptr) {
        return false;
    }
    static const jmethodID session_ctor = env->GetMethodID(
        session_cls, "<init>",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    adjust::jni::clear_exception(jni);
    if (session_ctor == nullptr) {
        return false;
    }

    // Reflection handles (cached).
    static const jclass class_cls = adjust::jni::find_class(jni, "java/lang/Class", "java/lang/Class");
    static const jclass field_cls = adjust::jni::find_class(jni, "java/lang/reflect/Field",
                                                            "java/lang/reflect/Field");
    if (class_cls == nullptr || field_cls == nullptr) {
        return false;
    }
    static const jmethodID get_declared_fields =
        env->GetMethodID(class_cls, "getDeclaredFields", "()[Ljava/lang/reflect/Field;");
    static const jmethodID field_get_type =
        env->GetMethodID(field_cls, "getType", "()Ljava/lang/Class;");
    static const jmethodID field_set_accessible = env->GetMethodID(field_cls, "setAccessible", "(Z)V");
    static const jmethodID field_set =
        env->GetMethodID(field_cls, "set", "(Ljava/lang/Object;Ljava/lang/Object;)V");
    adjust::jni::clear_exception(jni);
    if (get_declared_fields == nullptr || field_get_type == nullptr || field_set_accessible == nullptr ||
        field_set == nullptr) {
        return false;
    }

    if (env->PushLocalFrame(16) != 0) {
        env->ExceptionClear();
        return false;
    }
    bool applied = false;

    // Build the new Session(username, playerID(uuid, no dashes), token, type).
    const std::string uuid = strip_dashes(account.uuid);
    const char* type = (account.type == AccountType::Offline) ? "legacy" : "mojang";
    jstring j_name = env->NewStringUTF(account.name.c_str());
    jstring j_uuid = env->NewStringUTF(uuid.c_str());
    jstring j_token = env->NewStringUTF(account.token.c_str());
    jstring j_type = env->NewStringUTF(type);
    jobject session = env->NewObject(session_cls, session_ctor, j_name, j_uuid, j_token, j_type);
    if (!adjust::jni::clear_exception(jni) && session != nullptr) {
        jobjectArray fields =
            static_cast<jobjectArray>(env->CallObjectMethod(mc_cls, get_declared_fields));
        if (!adjust::jni::clear_exception(jni) && fields != nullptr) {
            const jsize count = env->GetArrayLength(fields);
            for (jsize i = 0; i < count && !applied; ++i) {
                jobject field = env->GetObjectArrayElement(fields, i);
                if (field == nullptr) {
                    continue;
                }
                jobject type_cls = env->CallObjectMethod(field, field_get_type);
                if (!adjust::jni::clear_exception(jni) && type_cls != nullptr &&
                    env->IsSameObject(type_cls, session_cls)) {
                    env->CallVoidMethod(field, field_set_accessible, JNI_TRUE);
                    adjust::jni::clear_exception(jni);
                    env->CallVoidMethod(field, field_set, minecraft, session);
                    applied = !adjust::jni::clear_exception(jni);
                }
                if (type_cls != nullptr) {
                    env->DeleteLocalRef(type_cls);
                }
                env->DeleteLocalRef(field);
            }
        }
    }

    env->PopLocalFrame(nullptr);
    return applied;
}

} // namespace slinky::altmanager
