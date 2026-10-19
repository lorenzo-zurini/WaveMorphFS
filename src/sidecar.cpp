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

std::optional<std::string> normalize_target(std::string_view p) {
    std::string out;
    for (auto& c : split(p, '/')) {
        if (c.empty()) continue;
        if (c == "." || c == ".." || c[0] == '.') return std::nullopt;
        if (!out.empty()) out += '/';
        out += c;
    }
    if (out.empty()) return std::nullopt;
    return out;
}

std::optional<std::string> canonical_track_key(std::string_view k) {
    auto sep = k.find_first_of("-.");
    if (sep == std::string_view::npos) {
        auto n = parse_u64(std::string(k));
        if (!n) return std::nullopt;
        return std::to_string(*n);
    }
    auto d = parse_u64(std::string(k.substr(0, sep))), n = parse_u64(std::string(k.substr(sep + 1)));
    if (!d || !n) return std::nullopt;
    return std::format("{}-{:02}", *d, *n);
}

std::string track_key(std::optional<uint32_t> disc, uint32_t n) { return disc ? std::format("{}-{:02}", *disc, n) : std::to_string(n); }

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
    for (const json* where : {&doc, doc.contains("album") ? &doc["album"] : nullptr})
        if (where && where->is_object())
            if (auto c = where->find("_cover"); c != where->end() && c->is_string() && !c->get<std::string>().empty()) sc.cover = c->get<std::string>();
    if (auto t = doc.find("_target"); t != doc.end() && t->is_string()) {
        sc.target = normalize_target(t->get<std::string>());
        if (!sc.target) warn("sidecar: ignoring invalid _target '{}'", t->get<std::string>());
    }
    if (auto h = doc.find("_hide"); h != doc.end()) {
        if (h->is_string()) sc.hidden.insert(h->get<std::string>());
        else if (h->is_array())
            for (auto& x : *h)
                if (x.is_string()) sc.hidden.insert(x.get<std::string>());
    }
    auto str = [](const json& t, const char* k) -> std::optional<std::string> {
        auto it = t.find(k);
        if (it == t.end() || !it->is_string() || it->get<std::string>().empty()) return std::nullopt;
        return it->get<std::string>();
    };
    auto target_of = [](const json& t) -> std::optional<std::string> {
        auto it = t.find("_target");
        if (it == t.end() || !it->is_string()) return std::nullopt;
        auto nt = normalize_target(it->get<std::string>());
        if (!nt) warn("sidecar: ignoring invalid _target '{}'", it->get<std::string>());
        return nt;
    };
    // track tables are stored under their canonical key, so that "05" and "5" (or
    // "2.5" and "2-05") are one table, as they are one track; later ones win
    if (auto s = doc.find("track"); s != doc.end() && s->is_object())
        for (auto& [k, v] : s->items()) {
            if (!v.is_object()) continue;
            std::string ck = canonical_track_key(k).value_or(k);
            sc.tracks[ck].merge(table_to_tags(v));
            if (auto n = str(v, "_name")) sc.track_names[ck] = *n;
            if (auto h = v.find("_hide"); h != v.end() && truthy(*h)) sc.hidden_tracks.insert(ck);
            if (auto c = str(v, "_cover")) sc.track_covers[ck] = *c;
            if (auto t = target_of(v)) sc.track_targets[ck] = *t;
        }
    if (auto s = doc.find("file"); s != doc.end() && s->is_object())
        for (auto& [k, v] : s->items()) {
            if (!v.is_object()) continue;
            sc.files[k] = table_to_tags(v);
            if (auto h = v.find("_hide"); h != v.end() && truthy(*h)) sc.hidden.insert(k);
            if (auto c = str(v, "_cover")) sc.file_covers[k] = *c;
            if (auto t = target_of(v)) sc.file_targets[k] = *t;
        }
    return sc;
}

void Sidecar::merge(const Sidecar& later, int64_t later_mtime) {
    // merge, not overlay: removals ("KEY": "") must survive to be applied
    album.merge(later.album);
    for (auto& [k, v] : later.tracks) tracks[k].merge(v);
    for (auto& [k, v] : later.files) files[k].merge(v);
    for (auto& [k, v] : later.track_names) track_names[k] = v;
    hidden.insert(later.hidden.begin(), later.hidden.end());
    hidden_tracks.insert(later.hidden_tracks.begin(), later.hidden_tracks.end());
    // "_target"s are not merged: they are read from the tags tree only (placement.cpp)
    if (later.cover) cover = later.cover;
    for (auto& [k, v] : later.track_covers) track_covers[k] = v;
    for (auto& [k, v] : later.file_covers) file_covers[k] = v;
    mtime = std::max(mtime.value_or(later_mtime), later_mtime);
}

std::vector<fs::path> Sidecar::paths(const fs::path& src_dir, const std::optional<fs::path>& overlay_dir) {
    std::vector<fs::path> out = {src_dir / SIDECAR_NAME};
    if (overlay_dir) out.push_back(*overlay_dir / SIDECAR_NAME);
    return out;
}

std::optional<Sidecar> Sidecar::load(const fs::path& src_dir, const std::optional<fs::path>& overlay_dir) {
    std::optional<Sidecar> out;
    for (auto& p : paths(src_dir, overlay_dir)) {
        auto m = mtime_ns(p);
        if (!m) continue;
        auto text = try_read_text(p);
        WM_ENSURE(text.has_value(), "read {}", p.string());
        Sidecar sc = with_context("parse " + p.string(), [&] { return parse(*text); });
        if (!out) out.emplace();
        out->merge(sc, *m);
    }
    return out;
}

const Tags* Sidecar::track(std::optional<uint32_t> disc, uint32_t n) const {
    auto it = tracks.find(track_key(disc, n));
    return it == tracks.end() ? nullptr : &it->second;
}

const std::string* Sidecar::track_name(std::optional<uint32_t> disc, uint32_t n) const {
    auto it = track_names.find(track_key(disc, n));
    return it == track_names.end() ? nullptr : &it->second;
}

const std::string* Sidecar::cover_for(const std::string& name, const std::string* track_key) const {
    if (auto it = file_covers.find(name); it != file_covers.end()) return &it->second;
    if (track_key)
        if (auto it = track_covers.find(*track_key); it != track_covers.end()) return &it->second;
    return cover ? &*cover : nullptr;
}

bool Sidecar::hides_track(std::optional<uint32_t> disc, uint32_t n) const { return hidden_tracks.contains(track_key(disc, n)); }

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
