// SPDX-License-Identifier: GPL-3.0-or-later
#include "sidecar.hpp"

#include <nlohmann/json.hpp>

namespace wm {

using json = nlohmann::ordered_json;

static std::string value_str(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_null()) return "";
    return v.dump();
}

static Tags table_to_tags(const json& t) {
    Tags tags;
    if (!t.is_object()) return tags;
    for (auto& [k, v] : t.items()) {
        if (!k.empty() && k[0] == '_') continue;  // not a tag
        std::vector<std::string> vals;
        if (v.is_array())
            for (auto& x : v) vals.push_back(value_str(x));
        else
            vals.push_back(value_str(v));
        std::erase_if(vals, [](auto& s) { return s.empty(); });
        // names are kept exactly as written; explicit removals stay as an empty
        // list so that overlay() deletes the key
        tags.m.erase(k);
        tags.m.emplace(k, std::move(vals));
    }
    return tags;
}

Sidecar Sidecar::parse(const std::string& text) {
    json doc = json::parse(text, nullptr, true, /*ignore_comments=*/true);
    WM_ENSURE(doc.is_object(), "sidecar is not a JSON object");
    Sidecar sc;
    if (auto a = doc.find("album"); a != doc.end()) sc.album = table_to_tags(*a);
    auto truthy = [](const json& v) {
        if (v.is_boolean()) return v.get<bool>();
        if (v.is_number()) return v.get<double>() != 0;
        if (v.is_string()) {
            std::string s = lower(trim(v.get<std::string>()));
            return s == "true" || s == "yes" || s == "1";
        }
        return false;
    };
    if (auto h = doc.find("_hide"); h != doc.end()) {
        if (h->is_string()) sc.hidden.insert(h->get<std::string>());
        else if (h->is_array())
            for (auto& x : *h)
                if (x.is_string()) sc.hidden.insert(x.get<std::string>());
    }
    for (auto [section, target] : {std::pair{"track", &sc.tracks}, std::pair{"file", &sc.files}}) {
        auto s = doc.find(section);
        if (s == doc.end() || !s->is_object()) continue;
        for (auto& [k, v] : s->items()) {
            if (!v.is_object()) continue;
            (*target)[k] = table_to_tags(v);
            if (target == &sc.tracks)
                if (auto n = v.find("_name"); n != v.end() && n->is_string() && !n->get<std::string>().empty()) sc.track_names[k] = n->get<std::string>();
            if (auto h = v.find("_hide"); h != v.end() && truthy(*h)) (target == &sc.tracks ? sc.hidden_tracks : sc.hidden).insert(k);
        }
    }
    return sc;
}

std::optional<Sidecar> Sidecar::load(const fs::path& src_dir, const std::optional<fs::path>& overlay_dir) {
    std::optional<Sidecar> out;
    std::vector<fs::path> candidates = {src_dir / SIDECAR_NAME};
    if (overlay_dir) candidates.push_back(*overlay_dir / SIDECAR_NAME);
    for (auto& p : candidates) {
        auto m = mtime_ns(p);
        if (!m) continue;
        auto text = try_read_text(p);
        WM_ENSURE(text.has_value(), "read {}", p.string());
        Sidecar sc = with_context("parse " + p.string(), [&] { return parse(*text); });
        if (!out) out.emplace();
        // merge, not overlay: removals ("KEY": "") must survive to be applied
        out->album.merge(sc.album);
        for (auto& [k, v] : sc.tracks) out->tracks[k].merge(v);
        for (auto& [k, v] : sc.files) out->files[k].merge(v);
        for (auto& [k, v] : sc.track_names) out->track_names[k] = v;
        out->hidden.insert(sc.hidden.begin(), sc.hidden.end());
        out->hidden_tracks.insert(sc.hidden_tracks.begin(), sc.hidden_tracks.end());
        out->mtime = std::max(out->mtime.value_or(*m), *m);
        out->sources.push_back(p);
    }
    return out;
}

const Tags* Sidecar::track(std::optional<uint32_t> disc, uint32_t n) const {
    std::vector<std::string> keys;
    if (disc) {
        keys = {std::format("{}-{}", *disc, n), std::format("{}-{:02}", *disc, n), std::format("{}.{}", *disc, n)};
    } else {
        keys = {std::to_string(n), std::format("{:02}", n)};
    }
    for (auto& k : keys)
        if (auto it = tracks.find(k); it != tracks.end()) return &it->second;
    return nullptr;
}

const std::string* Sidecar::track_name(std::optional<uint32_t> disc, uint32_t n) const {
    std::vector<std::string> keys;
    if (disc) keys = {std::format("{}-{}", *disc, n), std::format("{}-{:02}", *disc, n), std::format("{}.{}", *disc, n)};
    else keys = {std::to_string(n), std::format("{:02}", n)};
    for (auto& k : keys)
        if (auto it = track_names.find(k); it != track_names.end()) return &it->second;
    return nullptr;
}

bool Sidecar::hides_track(std::optional<uint32_t> disc, uint32_t n) const {
    std::vector<std::string> keys;
    if (disc) keys = {std::format("{}-{}", *disc, n), std::format("{}-{:02}", *disc, n), std::format("{}.{}", *disc, n)};
    else keys = {std::to_string(n), std::format("{:02}", n)};
    for (auto& k : keys)
        if (hidden_tracks.contains(k)) return true;
    return false;
}

const Tags* Sidecar::file(const std::string& name) const {
    auto it = files.find(name);
    return it == files.end() ? nullptr : &it->second;
}

std::string Sidecar::render(const Tags& album, const std::vector<std::pair<std::string, Tags>>& tracks,
                            const std::vector<std::pair<std::string, Tags>>& files) {
    auto table = [](const Tags& t) {
        json o = json::object();
        for (auto& [k, v] : t.m) o[k] = v.size() == 1 ? json(v[0]) : json(v);
        return o;
    };
    json doc = json::object();
    doc["_comment"] = "WaveMorphFS sidecar tags - edit freely; the filesystem picks up changes within seconds.";
    doc["album"] = table(album);
    if (!tracks.empty()) {
        json t = json::object();
        for (auto& [k, v] : tracks) t[k] = table(v);
        doc["track"] = t;
    }
    if (!files.empty()) {
        json f = json::object();
        for (auto& [k, v] : files) f[k] = table(v);
        doc["file"] = f;
    }
    return doc.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
}

}  // namespace wm
