#include "anim_clip.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace tubane::sandbox {

using namespace DirectX;

namespace {

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool StartsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

// "{x: 1, y: 2, z: 3, w: 4}" -> values in x,y,z,w order; returns component count.
int ParseVector(const std::string& text, float out[4]) {
    static const char* kKeys[4] = {"x:", "y:", "z:", "w:"};
    int count = 0;
    for (int i = 0; i < 4; ++i) {
        const size_t at = text.find(kKeys[i]);
        if (at == std::string::npos) break;
        out[i] = std::strtof(text.c_str() + at + 2, nullptr);
        count = i + 1;
    }
    return count;
}

// Last path segment, lower-case, alphanumerics only ("Armature/Left_Hand" -> "lefthand").
std::string NormalizedName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    const std::string last = slash == std::string::npos ? path : path.substr(slash + 1);
    std::string out;
    for (char c : last) {
        if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

struct Key {
    float time;
    float value[4];
    int   count;
};

struct Entry {
    std::string      path;
    std::string      attribute;
    std::vector<Key> keys;
};

void AddComponentCurves(AnimClip& clip, const std::string& prefix, const Entry& e, int components, bool onlyIfMissing) {
    static const char kAxis[4] = {'x', 'y', 'z', 'w'};
    for (int c = 0; c < components; ++c) {
        const std::string key = prefix + e.path + "." + kAxis[c];
        if (onlyIfMissing && clip.curves.count(key)) continue;
        FloatCurve& curve = clip.curves[key];
        curve.times.clear();
        curve.values.clear();
        for (const Key& k : e.keys) {
            if (k.count <= c) continue;
            curve.times.push_back(k.time);
            curve.values.push_back(k.value[c]);
        }
    }
}

void AddFloatCurve(AnimClip& clip, const std::string& key, const Entry& e, bool onlyIfMissing) {
    if (onlyIfMissing && clip.curves.count(key)) return;
    FloatCurve& curve = clip.curves[key];
    curve.times.clear();
    curve.values.clear();
    for (const Key& k : e.keys) {
        curve.times.push_back(k.time);
        curve.values.push_back(k.value[0]);
    }
}

void FinalizeEntry(AnimClip& clip, const std::string& section, Entry& e) {
    if (e.keys.empty()) return;
    if (section == "m_PositionCurves") {
        AddComponentCurves(clip, "pos:", e, 3, false);
    } else if (section == "m_RotationCurves") {
        AddComponentCurves(clip, "rot:", e, 4, false);
    } else if (section == "m_EulerCurves") {
        AddComponentCurves(clip, "eul:", e, 3, false);
    } else if (section == "m_FloatCurves") {
        // Humanoid muscle / goal curves have an empty path.
        AddFloatCurve(clip, "float:" + e.attribute, e, false);
    } else if (section == "m_EditorCurves" || section == "m_EulerEditorCurves") {
        // Per-component editor curves; used only when the runtime curves are absent.
        const std::string& a = e.attribute;
        const size_t dot = a.find_last_of('.');
        if (dot == std::string::npos) return;
        const std::string prop = a.substr(0, dot);
        const std::string axis = a.substr(dot + 1);
        std::string prefix;
        if (prop == "m_LocalPosition") prefix = "pos:";
        else if (prop == "m_LocalRotation") prefix = "rot:";
        else if (StartsWith(prop, "localEulerAngles")) prefix = "eul:";
        if (prefix.empty()) return;
        AddFloatCurve(clip, prefix + e.path + "." + axis, e, true);
    }
}

// Unity (left-handed, +Z forward) -> OpenVR (right-handed, -Z forward).
XMVECTOR UnityToOpenVrPosition(float x, float y, float z) { return XMVectorSet(x, y, -z, 0.0f); }
XMVECTOR UnityToOpenVrRotation(float x, float y, float z, float w) {
    return XMQuaternionNormalize(XMVectorSet(-x, -y, z, w));
}

// Device-name patterns for generic clips (normalized names).
const std::vector<std::vector<std::string>>& GenericNamePatterns() {
    static const std::vector<std::vector<std::string>> kPatterns = {
        /* HMD   */ {"head", "hmd", "headset", "camera", "maincamera", "centereyeanchor", "eye"},
        /* L     */ {"lefthand", "handl", "lhand", "leftcontroller", "controllerleft", "lefthandanchor", "lefthandcontroller"},
        /* R     */ {"righthand", "handr", "rhand", "rightcontroller", "controllerright", "righthandanchor", "righthandcontroller"},
        /* waist */ {"hips", "hip", "waist", "pelvis", "waisttracker"},
        /* LFoot */ {"leftfoot", "footl", "lfoot", "leftankle", "leftfoottracker"},
        /* RFoot */ {"rightfoot", "footr", "rfoot", "rightankle", "rightfoottracker"},
        /* chest */ {"chest", "upperchest", "chesttracker"},
        /* LElb  */ {"leftelbow", "leftlowerarm", "elbowl"},
        /* RElb  */ {"rightelbow", "rightlowerarm", "elbowr"},
        /* LKnee */ {"leftknee", "leftlowerleg", "kneel"},
        /* RKnee */ {"rightknee", "rightlowerleg", "kneer"},
    };
    return kPatterns;
}

void BindHumanoid(AnimClip& clip) {
    struct Goal { uint32_t slot; const char* name; };
    const Goal goals[] = {
        {ipc::TrackerSlot(0), "Root"},
        {ipc::kSlotLeftHand, "LeftHand"},
        {ipc::kSlotRightHand, "RightHand"},
        {ipc::TrackerSlot(1), "LeftFoot"},
        {ipc::TrackerSlot(2), "RightFoot"},
    };
    static const char kAxis[4] = {'x', 'y', 'z', 'w'};
    for (const Goal& g : goals) {
        DeviceBinding& b = clip.bindings[g.slot];
        b.hasPosition = b.hasRotation = true;
        for (int c = 0; c < 3; ++c) b.posKey[c] = std::string("float:") + g.name + "T." + kAxis[c];
        for (int c = 0; c < 4; ++c) b.rotKey[c] = std::string("float:") + g.name + "Q." + kAxis[c];
        for (int c = 0; c < 3; ++c) b.hasPosition = b.hasPosition && clip.curves.count(b.posKey[c]);
        for (int c = 0; c < 4; ++c) b.hasRotation = b.hasRotation && clip.curves.count(b.rotKey[c]);
        b.source = std::string(g.name) + "T/Q";
    }
    clip.hmdEstimated = clip.bindings[ipc::TrackerSlot(0)].hasPosition && clip.bindings[ipc::TrackerSlot(0)].hasRotation;
    if (clip.hmdEstimated) clip.bindings[ipc::kSlotHmd].source = "estimated from RootT/RootQ";
}

void BindGeneric(AnimClip& clip) {
    // Collect object paths that have transform curves.
    std::map<std::string, int> paths;  // path -> depth
    for (const auto& [key, curve] : clip.curves) {
        const size_t colon = key.find(':');
        const size_t dot   = key.find_last_of('.');
        if (colon == std::string::npos || dot == std::string::npos || dot < colon) continue;
        const std::string kind = key.substr(0, colon);
        if (kind != "pos" && kind != "rot" && kind != "eul") continue;
        const std::string path = key.substr(colon + 1, dot - colon - 1);
        paths[path] = static_cast<int>(std::count(path.begin(), path.end(), '/'));
    }

    static const char kAxis[4] = {'x', 'y', 'z', 'w'};
    const auto& patterns = GenericNamePatterns();
    for (uint32_t slot = 0; slot < ipc::kMaxDevices && slot < patterns.size(); ++slot) {
        // Prefer the shallowest matching path.
        std::string best;
        int         bestDepth = 1 << 30;
        for (const auto& [path, depth] : paths) {
            const std::string n = NormalizedName(path);
            if (std::find(patterns[slot].begin(), patterns[slot].end(), n) == patterns[slot].end()) continue;
            if (depth < bestDepth) {
                best      = path;
                bestDepth = depth;
            }
        }
        if (best.empty() && !(slot == 0 && paths.count(""))) continue;

        DeviceBinding& b = clip.bindings[slot];
        b.source = best.empty() ? "(root)" : best;
        b.hasPosition = true;
        for (int c = 0; c < 3; ++c) {
            b.posKey[c] = "pos:" + best + "." + kAxis[c];
            b.hasPosition = b.hasPosition && clip.curves.count(b.posKey[c]);
        }
        b.hasRotation = true;
        for (int c = 0; c < 4; ++c) {
            b.rotKey[c] = "rot:" + best + "." + kAxis[c];
            b.hasRotation = b.hasRotation && clip.curves.count(b.rotKey[c]);
        }
        if (!b.hasRotation) {
            // Euler curves (degrees): stored with a 3-key "eul:" marker in rotKey.
            bool euler = true;
            for (int c = 0; c < 3; ++c) euler = euler && clip.curves.count("eul:" + best + "." + kAxis[c]);
            if (euler) {
                for (int c = 0; c < 3; ++c) b.rotKey[c] = "eul:" + best + "." + kAxis[c];
                b.rotKey[3].clear();
                b.hasRotation = true;
            }
        }
        if (bestDepth > 0 && bestDepth < (1 << 30)) {
            clip.warnings.push_back("'" + best + "' is nested; its local transform is used as world transform");
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
float FloatCurve::Evaluate(float t) const {
    if (times.empty()) return 0.0f;
    if (t <= times.front()) return values.front();
    if (t >= times.back()) return values.back();
    const auto it = std::upper_bound(times.begin(), times.end(), t);
    const size_t i1 = static_cast<size_t>(it - times.begin());
    const size_t i0 = i1 - 1;
    const float  span = times[i1] - times[i0];
    const float  a = span > 1e-9f ? (t - times[i0]) / span : 0.0f;
    return values[i0] + (values[i1] - values[i0]) * a;
}

bool AnimClip::Empty() const {
    if (hmdEstimated) return false;
    for (const DeviceBinding& b : bindings) {
        if (b.hasPosition || b.hasRotation) return false;
    }
    return true;
}

bool LoadUnityAnim(const std::filesystem::path& path, AnimClip& clip, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open file";
        return false;
    }
    std::string first;
    std::getline(in, first);
    if (!StartsWith(first, "%YAML")) {
        error = "not a text (YAML) .anim file - set Unity's Asset Serialization to 'Force Text'";
        return false;
    }

    AnimClip    result;
    std::string section;
    Entry       entry;
    bool        inEntry = false;
    float       pendingTime = 0.0f;
    bool        compressed = false;

    auto finalize = [&]() {
        if (inEntry) FinalizeEntry(result, section, entry);
        entry   = Entry{};
        inEntry = false;
    };

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t indent = 0;
        while (indent < line.size() && line[indent] == ' ') ++indent;
        const std::string t = Trim(line);
        if (t.empty()) continue;

        // Top-level AnimationClip fields live at indent 2: "  m_Xxx: ..."
        if (indent == 2 && StartsWith(t, "m_")) {
            finalize();
            const size_t colon = t.find(':');
            section = t.substr(0, colon);
            const std::string value = colon == std::string::npos ? "" : Trim(t.substr(colon + 1));
            if (section == "m_Name") result.name = value;
            if (section == "m_Compressed" && value == "1") compressed = true;
            continue;
        }
        if (indent == 2 && t == "- curve:") {
            finalize();
            inEntry = true;
            continue;
        }
        if (!inEntry) continue;

        if (StartsWith(t, "time:")) {
            pendingTime = std::strtof(t.c_str() + 5, nullptr);
        } else if (StartsWith(t, "value:")) {
            Key k{};
            k.time = pendingTime;
            const std::string v = Trim(t.substr(6));
            if (!v.empty() && v.front() == '{') {
                k.count = ParseVector(v, k.value);
            } else {
                k.value[0] = std::strtof(v.c_str(), nullptr);
                k.count    = 1;
            }
            if (k.count > 0) entry.keys.push_back(k);
        } else if (StartsWith(t, "path:")) {
            entry.path = Trim(t.substr(5));
        } else if (StartsWith(t, "attribute:")) {
            entry.attribute = Trim(t.substr(10));
        }
    }
    finalize();

    if (compressed) {
        result.warnings.push_back("clip is compressed (m_Compressed: 1); compressed curves are not supported");
    }

    for (const auto& [key, curve] : result.curves) {
        if (!curve.times.empty()) result.duration = std::max(result.duration, curve.times.back());
    }

    if (result.curves.count("float:RootT.x")) {
        result.kind = AnimClip::Kind::Humanoid;
        BindHumanoid(result);
    } else {
        result.kind = AnimClip::Kind::Generic;
        BindGeneric(result);
    }
    if (result.Empty()) {
        error = result.kind == AnimClip::Kind::Humanoid
                    ? "humanoid clip without IK goal curves"
                    : "no curves for objects named like devices (Head / LeftHand / RightHand / Hips / LeftFoot / RightFoot ...)";
        return false;
    }
    if (result.name.empty()) result.name = path.stem().string();
    clip = std::move(result);
    return true;
}

// ---------------------------------------------------------------------------
void SampleClip(const AnimClip& clip, float t, const AnimSampleOptions& o, ipc::DevicePose (&poses)[ipc::kMaxDevices],
                bool (&driven)[ipc::kMaxDevices]) {
    const XMVECTOR yawQ   = XMQuaternionRotationRollPitchYaw(0.0f, o.yaw, 0.0f);
    const XMMATRIX yawM   = XMMatrixRotationY(o.yaw);
    const XMVECTOR offset = XMLoadFloat3(&o.offset);

    auto curve = [&](const std::string& key) {
        const auto it = clip.curves.find(key);
        return it == clip.curves.end() ? 0.0f : it->second.Evaluate(t);
    };
    auto store = [&](uint32_t slot, XMVECTOR posOvr, bool hasPos, XMVECTOR rotOvr, bool hasRot) {
        ipc::DevicePose& p = poses[slot];
        if (hasPos) {
            XMFLOAT3 w;
            XMStoreFloat3(&w, XMVectorAdd(XMVector3TransformCoord(XMVectorScale(posOvr, o.scale), yawM), offset));
            p.position[0] = w.x;
            p.position[1] = w.y;
            p.position[2] = w.z;
        }
        if (hasRot) {
            XMFLOAT4 q;
            XMStoreFloat4(&q, XMQuaternionNormalize(XMQuaternionMultiply(rotOvr, yawQ)));  // rot first, then yaw
            p.rotation[0] = q.w;
            p.rotation[1] = q.x;
            p.rotation[2] = q.y;
            p.rotation[3] = q.z;
        }
        driven[slot] = hasPos || hasRot;
    };

    for (uint32_t slot = 0; slot < ipc::kMaxDevices; ++slot) {
        driven[slot] = false;
        const DeviceBinding& b = clip.bindings[slot];
        if (!b.hasPosition && !b.hasRotation) continue;

        XMVECTOR pos = XMVectorZero();
        if (b.hasPosition) pos = UnityToOpenVrPosition(curve(b.posKey[0]), curve(b.posKey[1]), curve(b.posKey[2]));

        XMVECTOR rot = XMQuaternionIdentity();
        if (b.hasRotation) {
            if (b.rotKey[3].empty()) {
                // Unity Euler degrees, applied Z -> X -> Y (same order as RollPitchYaw).
                const XMVECTOR qu = XMQuaternionRotationRollPitchYaw(XMConvertToRadians(curve(b.rotKey[0])),
                                                                     XMConvertToRadians(curve(b.rotKey[1])),
                                                                     XMConvertToRadians(curve(b.rotKey[2])));
                XMFLOAT4 f;
                XMStoreFloat4(&f, qu);
                rot = UnityToOpenVrRotation(f.x, f.y, f.z, f.w);
            } else {
                rot = UnityToOpenVrRotation(curve(b.rotKey[0]), curve(b.rotKey[1]), curve(b.rotKey[2]), curve(b.rotKey[3]));
            }
        }
        store(slot, pos, b.hasPosition, rot, b.hasRotation);
    }

    if (clip.hmdEstimated) {
        const DeviceBinding& root = clip.bindings[ipc::TrackerSlot(0)];
        const XMVECTOR rootPos = UnityToOpenVrPosition(curve(root.posKey[0]), curve(root.posKey[1]), curve(root.posKey[2]));
        const XMVECTOR rootRot = UnityToOpenVrRotation(curve(root.rotKey[0]), curve(root.rotKey[1]), curve(root.rotKey[2]),
                                                       curve(root.rotKey[3]));
        const XMVECTOR head = XMVectorAdd(rootPos, XMVector3Rotate(XMVectorSet(0.0f, o.headOffset, 0.0f, 0.0f), rootRot));
        store(ipc::kSlotHmd, head, true, rootRot, true);
    }
}

}  // namespace tubane::sandbox
