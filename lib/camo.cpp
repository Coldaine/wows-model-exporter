/* camouflage.cpp — XML-driven camouflage material variants spike.
 *
 * Parses camouflages.xml, resolves per-ship camouflage entries, classifies
 * MFM stems into part categories, bakes tiled DDS colour masks, applies
 * KHR_texture_transform UV offsets, and emits KHR_materials_variants blocks.
 */

#include "wows-camo.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <tiny_gltf.h>

#define vlog(tag, fmt, ...)                                                                                            \
    do {                                                                                                               \
        if (wows_stitch_verbose)                                                                                       \
            fprintf(stderr, "[%s] " fmt, tag, ##__VA_ARGS__);                                                          \
    } while (0)

/* ── Minimal XML pull parser ──────────────────────────────────────── */

struct xml_ev {
    enum { TEXT, OPEN, CLOSE, EMPTY } type;
    std::string name;        /* tag name (OPEN/EMPTY/CLOSE) */
    std::vector<std::pair<std::string, std::string>> attrs; /* OPEN/EMPTY only */
    std::string text;        /* TEXT only — text content */
};

static const char *xml_skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        ++p;
    return p;
}

static const char *xml_read_name(const char *p, const char *end, std::string *out) {
    out->clear();
    while (p < end &&
           ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
            *p == '_' || *p == ':' || *p == '.' || *p == '-')) {
        out->push_back(*p++);
    }
    return p;
}

static const char *xml_read_attr_value(const char *p, const char *end, std::string *out) {
    out->clear();
    if (p >= end || *p != '"')
        return p;
    ++p;
    while (p < end && *p != '"') {
        if (*p == '&') {
            ++p;
            if (p < end && *p == 'q') {
                if (p + 5 <= end && memcmp(p, "quot;", 5) == 0) { out->push_back('"'); p += 5; continue; }
            }
            if (p < end && *p == 'a') {
                if (p + 5 <= end && memcmp(p, "mp;", 3) == 0) { out->push_back('&'); p += 5; continue; }
            }
            out->push_back('&');
        } else {
            out->push_back(*p++);
        }
    }
    if (p < end && *p == '"') ++p;
    return p;
}

static const char *xml_next_ev(const char *p, const char *end, xml_ev *ev) {
    p = xml_skip_ws(p, end);
    if (p >= end) {
        ev->type = xml_ev::CLOSE;
        ev->name.clear();
        return p;
    }
    if (p[0] == '<' && p[1] == '/') {
        ev->type = xml_ev::CLOSE;
        p += 2;
        p = xml_skip_ws(p, end);
        p = xml_read_name(p, end, &ev->name);
        p = xml_skip_ws(p, end);
        if (p < end && *p == '>') ++p;
        return p;
    }
    if (p[0] == '<' && p[1] == '!' && p + 2 < end && p[2] == '[') {
        /* Skip CDATA section entirely */
        const char *cd_end = std::search(p + 3, end, "]]>", std::string("]]>").end());
        return (cd_end == end) ? end : cd_end + 3;
    }
    if (p[0] == '<' && (p[1] == '?' || p[1] == '!')) {
        /* Skip processing instructions and declarations */
        const char *q_end = std::search(p + 2, end, "?>", std::string("?>").end());
        return (q_end == end) ? end : q_end + 2;
    }
    if (p[0] != '<') {
        ev->type = xml_ev::TEXT;
        ev->text.clear();
        while (p < end && *p != '<')
            ev->text.push_back(*p++);
        /* Trim whitespace-only text */
        bool has_non_ws = false;
        for (char c : ev->text) {
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') { has_non_ws = true; break; }
        }
        if (!has_non_ws) { ev->type = xml_ev::CLOSE; ev->name.clear(); }
        return p;
    }
    p++; /* past '<' */
    p = xml_read_name(p, end, &ev->name);
    ev->attrs.clear();
    while (true) {
        p = xml_skip_ws(p, end);
        if (p >= end) break;
        if (*p == '>') { ++p; break; }
        if (p + 1 < end && p[0] == '/' && p[1] == '>') { p += 2; ev->type = xml_ev::EMPTY; return p; }
        std::string aname;
        p = xml_read_name(p, end, &aname);
        if (aname.empty()) break;
        p = xml_skip_ws(p, end);
        if (p < end && *p == '=') {
            ++p;
            p = xml_skip_ws(p, end);
            std::string aval;
            p = xml_read_attr_value(p, end, &aval);
            ev->attrs.emplace_back(aname, aval);
        } else {
            ev->attrs.emplace_back(aname, "");
        }
    }
    ev->type = xml_ev::OPEN;
    return p;
}

static std::string xml_get_attr(const xml_ev &ev, const char *name, const char *def) {
    for (const auto &kv : ev.attrs)
        if (kv.first == name) return kv.second;
    return def;
}

static float xml_get_attr_f(const xml_ev &ev, const char *name, float def) {
    std::string s = xml_get_attr(ev, name, nullptr);
    if (s.empty()) return def;
    return std::atof(s.c_str());
}

static bool xml_get_attr_b(const xml_ev &ev, const char *name, bool def) {
    std::string s = xml_get_attr(ev, name, nullptr);
    if (s.empty()) return def;
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s == "1" || s == "true" || s == "yes";
}

/* ── Database implementation ───────────────────────────────────────── */

struct wows_camo_db_impl {
    std::vector<wows_camo_color_schema> color_schemas;
    std::vector<wows_camo_entry>        entries;
    std::vector<wows_camo_ship_group>   ship_groups;
};

extern "C" {

static wows_camo_db_impl *impl(wows_camo_db *db) {
    return reinterpret_cast<wows_camo_db_impl *>(db);
}

/* ── XML structure reader ─────────────────────────────────────────── */

/* Consume all children of an already-opened element.  CALLER must first
 * position *pp just AFTER the OPEN tag (past any immediate forwarding
 * whitespace/text that follows it).  On return *pp is placed just BEFORE
 * the matching CLOSE tag so that the SAME call can be re-invoked to read
 * the next sibling.                                                            */
static std::vector<xml_ev> read_subtree(const char **pp, const char *end) {
    std::vector<xml_ev> out;
    const char *p = *pp;
    int depth = 1;
    while (p < end) {
        xml_ev ev;
        p = xml_next_ev(p, end, &ev);
        if (ev.type == xml_ev::CLOSE) {
            if (--depth == 0) { --p; *pp = p; return out; }  /* leave p one before CLOSE */
            continue;
        }
        if (ev.type != xml_ev::OPEN && ev.type != xml_ev::EMPTY && ev.type != xml_ev::TEXT) continue;
        if (ev.type == xml_ev::OPEN)
            ++depth;
        out.push_back(std::move(ev));
    }
    *pp = p;
    return out;
}

static wows_camo_db *parse_xml(const char *data, size_t size) {
    auto *db = new wows_camo_db_impl;
    const char *p = data, *end = data + size;

    /* Walk top-level elements directly.  read_subtree advances *pp past the
     * matching CLOSE tag, so the while loop naturally advances through siblings
     * without any secondary sibling-iteration step inside parse_xml.          */
    while (p < end) {
        xml_ev rev;
        p = xml_next_ev(p, end, &rev);
        if (rev.type == xml_ev::CLOSE) break;
        if (rev.type == xml_ev::TEXT) continue;
        if (rev.type != xml_ev::OPEN && rev.type != xml_ev::EMPTY) continue;

        if (rev.name == "colorSchemas") {
            auto kids = read_subtree(&p, end);
            for (const auto &cev : kids) {
                if (cev.type != xml_ev::OPEN && cev.type != xml_ev::EMPTY) continue;
                if (cev.name != "colorSchema") continue;
                wows_camo_color_schema cs;
                memset(&cs, 0, sizeof(cs));
                strncpy(cs.id,   xml_get_attr(cev, "id",   "").c_str(), sizeof(cs.id)   - 1);
                strncpy(cs.name, xml_get_attr(cev, "name", "").c_str(), sizeof(cs.name) - 1);
                auto ckids = read_subtree(&p, end);
                int ci = 0;
                for (const auto &kev : ckids) {
                    if (kev.type != xml_ev::OPEN || kev.name != "color") continue;
                    if (ci < 4) {
                        cs.colors[ci].r = xml_get_attr_f(kev, "r", 0.0f);
                        cs.colors[ci].g = xml_get_attr_f(kev, "g", 0.0f);
                        cs.colors[ci].b = xml_get_attr_f(kev, "b", 0.0f);
                        cs.colors[ci].a = xml_get_attr_f(kev, "a", 1.0f);
                        ++ci;
                    }
                }
                db->color_schemas.push_back(cs);
            }
        } else if (rev.name == "shipGroups") {
            auto kids = read_subtree(&p, end);
            for (const auto &gev : kids) {
                if (gev.type != xml_ev::OPEN && gev.type != xml_ev::EMPTY) continue;
                if (gev.name != "shipGroup") continue;
                wows_camo_ship_group sg;
                memset(&sg, 0, sizeof(sg));
                strncpy(sg.group_name, xml_get_attr(gev, "id", "").c_str(), sizeof(sg.group_name) - 1);
                auto gkids = read_subtree(&p, end);
                for (const auto &kev : gkids) {
                    if (kev.type != xml_ev::TEXT) continue;
                    std::istringstream iss(kev.text);
                    std::string token;
                    while (std::getline(iss, token, ',') && sg.n_camo_ids < 32) {
                        auto st = token.find_first_not_of(" \t\n\r");
                        auto en = token.find_last_not_of(" \t\n\r");
                        if (st == std::string::npos) continue;
                        token = token.substr(st, en - st + 1);
                        strncpy(sg.camo_ids[sg.n_camo_ids], token.c_str(), 63);
                        sg.camo_ids[sg.n_camo_ids][63] = '\0';
                        ++sg.n_camo_ids;
                    }
                }
                db->ship_groups.push_back(sg);
            }
        } else if (rev.name == "camouflages") {
            auto kids = read_subtree(&p, end);
            for (const auto &cev : kids) {
                if (cev.type != xml_ev::OPEN && cev.type != xml_ev::EMPTY) continue;
                if (cev.name != "camouflage") continue;
                wows_camo_entry ent;
                memset(&ent, 0, sizeof(ent));
                strncpy(ent.id,              xml_get_attr(cev, "id",    "").c_str(), sizeof(ent.id) - 1);
                strncpy(ent.name,            xml_get_attr(cev, "name",  "").c_str(), sizeof(ent.name) - 1);
                strncpy(ent.color_schema_id, xml_get_attr(cev, "colorScheme", "").c_str(), sizeof(ent.color_schema_id) - 1);
                ent.is_tiled = xml_get_attr_b(cev, "tile", false);
                ent.texture.n_categories = 0;
                ent.texture.n_uv_transforms = 0;

                auto ekids = read_subtree(&p, end);
                for (const auto &kev : ekids) {
                    if (kev.type != xml_ev::OPEN) continue;
                    if (kev.name == "texture") {
                        strncpy(ent.texture.color_mask_path, xml_get_attr(kev, "colorMask", "").c_str(),
                                sizeof(ent.texture.color_mask_path) - 1);
                        ent.texture.is_tiled = xml_get_attr_b(kev, "tile", false);
                        auto pkids = read_subtree(&p, end);
                        for (const auto &pev : pkids) {
                            if (pev.type != xml_ev::OPEN || pev.name != "part") continue;
                            if (ent.texture.n_categories >= 8) continue;
                            std::string cat_str = xml_get_attr(pev, "category", "misc");
                            wows_camo_part_category cat = WOWS_CAMO_PART_MISC;
                            std::transform(cat_str.begin(), cat_str.end(), cat_str.begin(), ::tolower);
                            if (cat_str == "hull_tile" || cat_str == "hull")        cat = WOWS_CAMO_PART_HULL_TILE;
                            else if (cat_str == "deckhouse" || cat_str == "deck")  cat = WOWS_CAMO_PART_DECKHOUSE;
                            else if (cat_str == "gun")                              cat = WOWS_CAMO_PART_GUN;
                            else if (cat_str == "director")                         cat = WOWS_CAMO_PART_DIRECTOR;
                            else if (cat_str == "bulge")                            cat = WOWS_CAMO_PART_BULGE;
                            ent.texture.categories[ent.texture.n_categories++] = cat;
                        }
                    } else if (kev.name == "uvTransform") {
                        if (ent.texture.n_uv_transforms >= 8) continue;
                        wows_camo_uv_transform &uv = ent.texture.uv_transforms[ent.texture.n_uv_transforms++];
                        std::string cat_str = xml_get_attr(kev, "partCategory", "misc");
                        std::transform(cat_str.begin(), cat_str.end(), cat_str.begin(), ::tolower);
                        if (cat_str == "hull_tile" || cat_str == "hull")        uv.category = WOWS_CAMO_PART_HULL_TILE;
                        else if (cat_str == "deckhouse" || cat_str == "deck")  uv.category = WOWS_CAMO_PART_DECKHOUSE;
                        else if (cat_str == "gun")                              uv.category = WOWS_CAMO_PART_GUN;
                        else if (cat_str == "director")                         uv.category = WOWS_CAMO_PART_DIRECTOR;
                        else if (cat_str == "bulge")                            uv.category = WOWS_CAMO_PART_BULGE;
                        else                                                    uv.category = WOWS_CAMO_PART_MISC;
                        uv.offset_u = xml_get_attr_f(kev, "offsetU", 0.0f);
                        uv.offset_v = xml_get_attr_f(kev, "offsetV", 0.0f);
                        uv.scale_u  = xml_get_attr_f(kev, "scaleU",  1.0f);
                        uv.scale_v  = xml_get_attr_f(kev, "scaleV",  1.0f);
                    }
                }
                if (ent.id[0]) db->entries.push_back(ent);
            }
        }
    }

    return db;
}

/* ── Public C API ──────────────────────────────────────────────────── */

wows_camo_db *wows_camo_open(const char *xml_path) {
    FILE *f = fopen(xml_path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return nullptr; }
    std::vector<char> buf((size_t)sz + 1);
    if (fread(buf.data(), 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return nullptr; }
    fclose(f);
    buf[sz] = '\0';
    wows_camo_db *db = parse_xml(buf.data(), (size_t)sz);
    if (!db) return nullptr;
    /* tag with a sentinel so free() works */
    return reinterpret_cast<wows_camo_db *>(db);
}

wows_camo_db *wows_camo_open_memory(const uint8_t *data, size_t size) {
    if (!data || size == 0) return nullptr;
    return reinterpret_cast<wows_camo_db *>(parse_xml(reinterpret_cast<const char *>(data), size));
}

void wows_camo_db_free(wows_camo_db *db) {
    if (!db) return;
    delete impl(db);
}

int wows_camo_color_schema_count(const wows_camo_db *db) {
    return (int)impl(db)->color_schemas.size();
}

const wows_camo_color_schema *wows_camo_color_schema_at(const wows_camo_db *db, int i) {
    const auto &v = impl(db)->color_schemas;
    if (i < 0 || i >= (int)v.size()) return nullptr;
    return &v[i];
}

const wows_camo_color_schema *wows_camo_find_color_schema(const wows_camo_db *db, const char *id) {
    if (!db || !id) return nullptr;
    for (const auto &cs : impl(db)->color_schemas)
        if (strcmp(cs.id, id) == 0) return &cs;
    return nullptr;
}

int wows_camo_entry_count(const wows_camo_db *db) {
    return (int)impl(db)->entries.size();
}

const wows_camo_entry *wows_camo_entry_at(const wows_camo_db *db, int i) {
    const auto &v = impl(db)->entries;
    if (i < 0 || i >= (int)v.size()) return nullptr;
    return &v[i];
}

const wows_camo_entry *wows_camo_find_entry(const wows_camo_db *db, const char *id) {
    if (!db || !id) return nullptr;
    for (const auto &e : impl(db)->entries)
        if (strcmp(e.id, id) == 0) return &e;
    return nullptr;
}

int wows_camo_ship_group_count(const wows_camo_db *db) {
    return (int)impl(db)->ship_groups.size();
}

const wows_camo_ship_group *wows_camo_ship_group_at(const wows_camo_db *db, int i) {
    const auto &v = impl(db)->ship_groups;
    if (i < 0 || i >= (int)v.size()) return nullptr;
    return &v[i];
}

const wows_camo_ship_group *wows_camo_find_ship_group(const wows_camo_db *db, const char *name) {
    if (!db || !name) return nullptr;
    for (const auto &sg : impl(db)->ship_groups)
        if (strcmp(sg.group_name, name) == 0) return &sg;
    return nullptr;
}

void wows_camo_resolve_for_group(const wows_camo_db *db, const char *ship_group,
                                 std::vector<std::string> *out_ids) {
    if (!db || !ship_group || !out_ids) return;
    out_ids->clear();
    const wows_camo_ship_group *sg = wows_camo_find_ship_group(db, ship_group);
    if (!sg) return;
    for (int i = 0; i < sg->n_camo_ids; ++i)
        out_ids->push_back(sg->camo_ids[i]);
}

/* ── Part classification ──────────────────────────────────────────── */

void wows_camo_classify_part(const char *mfm_stem, wows_camo_part_category *category) {
    if (!mfm_stem || !category) return;
    std::string s = mfm_stem;
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);

    bool hull_tile  = (s.find("_tile") != std::string::npos) || (s.find("hull")    != std::string::npos && !s.empty());
    bool deckhouse  = (s.find("deck") != std::string::npos) || s.rfind("dc_", 0) == 0 || s.rfind("dc", 0) == 0;
    bool gun        = (s.find("gun")  != std::string::npos) || s.rfind("gun_", 0) == 0;
    bool director   = (s.find("director") != std::string::npos) || s.rfind("dir_", 0) == 0;
    bool bulge      = (s.find("bulge") != std::string::npos)   || s.rfind("blg_", 0) == 0;

    if (hull_tile)  *category = WOWS_CAMO_PART_HULL_TILE;
    else if (deckhouse)  *category = WOWS_CAMO_PART_DECKHOUSE;
    else if (gun)       *category = WOWS_CAMO_PART_GUN;
    else if (director)  *category = WOWS_CAMO_PART_DIRECTOR;
    else if (bulge)     *category = WOWS_CAMO_PART_BULGE;
    else                *category = WOWS_CAMO_PART_MISC;
}

/* ── DDS baking helpers (uses dds.cpp internals) ──────────────────── */

static std::vector<uint8_t> bake_modulate(const std::vector<uint8_t> &rgba, int w, int h,
                                          const wows_camo_rgba scheme[4]) {
    std::vector<uint8_t> out = rgba;
    for (int i = 0; i < w * h; ++i) {
        uint8_t pal_idx = (uint8_t)((rgba[i*4] * 0.299f + rgba[i*4+1] * 0.587f + rgba[i*4+2] * 0.114f) / 255.0f * 3.0f);
        pal_idx = (int)pal_idx > 3 ? 3 : (int)pal_idx;
        const wows_camo_rgba &c = scheme[pal_idx];
        out[i*4+0] = (uint8_t)(std::min(255.0f, rgba[i*4+0] * c.r));
        out[i*4+1] = (uint8_t)(std::min(255.0f, rgba[i*4+1] * c.g));
        out[i*4+2] = (uint8_t)(std::min(255.0f, rgba[i*4+2] * c.b));
        out[i*4+3] = rgba[i*4+3];
    }
    return out;
}

extern "C" {
/* forward declaration; defined in dds.cpp */
std::vector<uint8_t> wows_stitch_decode_dds(const uint8_t *d, size_t sz, int *W, int *H);
}

/* We inline a minimal rgba->png encoder so camo.cpp is self-contained. */
#include "../deps/bcdec.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../deps/stb/stb_image_write.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "../deps/stb/stb_image_resize2.h"

static std::vector<uint8_t> rgba_to_png(std::vector<uint8_t> rgba, int w, int h, int max_sz) {
    if (max_sz > 0 && (w > max_sz || h > max_sz)) {
        float s = (float)max_sz / std::max(w, h);
        int nw = std::max(1, (int)(w * s)), nh = std::max(1, (int)(h * s));
        std::vector<uint8_t> rs((size_t)nw * nh * 4);
        stbir_resize_uint8_srgb(rgba.data(), w, h, 0, rs.data(), nw, nh, 0, STBIR_RGBA);
        rgba = std::move(rs);
        w = nw; h = nh;
    }
    std::vector<uint8_t> png;
    stbi_write_png_to_func(
        [](void *ctx, void *data, int n) {
            auto *v = (std::vector<uint8_t> *)ctx;
            const auto *p = (const uint8_t *)data;
            v->insert(v->end(), p, p + n);
        },
        &png, w, h, 4, rgba.data(), w * 4);
    return png;
}

/* Re-decode DDS, modulate with scheme, re-encode to PNG */
static std::vector<uint8_t> bake_dds_png(const uint8_t *data, size_t size,
                                          const wows_camo_rgba scheme[4], int max_sz) {
    int w, h;
    std::vector<uint8_t> rgba = wows_stitch_decode_dds(data, size, &w, &h);
    if (rgba.empty()) return {};
    /* Convert to luminance and look up palette colour */
    std::vector<uint8_t> baked = bake_modulate(rgba, w, h, scheme);
    return rgba_to_png(baked, w, h, max_sz);
}

/* ── Tiled baking public API ───────────────────────────────────────── */

static std::vector<uint8_t> load_and_bake(const std::string &mask_path,
                                          const wows_camo_rgba scheme[4],
                                          int max_sz,
                                          wows_file_provider_t file_provider,
                                          const std::string &game_dir) {
    auto try_file = [&](const std::string &p) -> std::vector<uint8_t> {
        FILE *f = fopen(p.c_str(), "rb");
        if (!f) return {};
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0) { fclose(f); return {}; }
        std::vector<uint8_t> raw((size_t)sz);
        if (fread(raw.data(), 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return {}; }
        fclose(f);
        return bake_dds_png(raw.data(), raw.size(), scheme, max_sz);
    };
    auto try_provider = [&](const std::string &rel) -> std::vector<uint8_t> {
        if (!file_provider) return {};
        auto buf = file_provider(rel);
        if (buf.empty()) return {};
        return bake_dds_png(buf.data(), buf.size(), scheme, max_sz);
    };

    std::string p = mask_path;
    /* try absolute path first */
    auto res = try_file(p);
    if (!res.empty()) return res;

    /* try relative to game_dir */
    std::string gp = game_dir + "/" + p;
    res = try_file(gp);
    if (!res.empty()) return res;

    /* try archive provider */
    std::string norm = wows_stitch_normalize_slashes(p);
    if (norm.size() > game_dir.size() && norm.compare(0, game_dir.size(), game_dir) == 0)
        norm = norm.substr(game_dir.size());
    if (!norm.empty() && norm[0] == '/') norm = norm.substr(1);
    return try_provider(norm);
}

std::vector<uint8_t> wows_camo_bake_tiled(const uint8_t *mask_data, size_t mask_size,
                                           const wows_camo_rgba schema[4], int max_sz) {
    if (!mask_data || !schema) return {};
    int w, h;
    std::vector<uint8_t> rgba = wows_stitch_decode_dds(mask_data, mask_size, &w, &h);
    if (rgba.empty()) return {};
    std::vector<uint8_t> baked = bake_modulate(rgba, w, h, schema);
    return rgba_to_png(baked, w, h, max_sz);
}

std::vector<uint8_t> wows_camo_bake_tiled_file(const char *mask_path, const wows_camo_rgba schema[4],
                                                int max_sz) {
    if (!mask_path || !schema) return {};
    FILE *f = fopen(mask_path, "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return {}; }
    std::vector<uint8_t> raw((size_t)sz);
    if (fread(raw.data(), 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return {}; }
    fclose(f);
    return bake_dds_png(raw.data(), raw.size(), schema, max_sz);
}

/* ── glTF extension helpers ───────────────────────────────────────── */

int wows_camo_embed_png(tinygltf::Model *model, const std::vector<uint8_t> &png) {
    if (!model || png.empty()) return -1;
    auto &buf = model->buffers[0].data;
    while (buf.size() & 3) buf.push_back(0);
    size_t boff = buf.size();
    buf.insert(buf.end(), png.begin(), png.end());
    tinygltf::BufferView bv;
    bv.buffer = 0;
    bv.byteOffset = (int)boff;
    bv.byteLength = (int)png.size();
    int bvi = (int)model->bufferViews.size();
    model->bufferViews.push_back(bv);

    tinygltf::Image img;
    img.bufferView = bvi;
    img.mimeType = "image/png";
    int ii = (int)model->images.size();
    model->images.push_back(img);

    tinygltf::Texture tex;
    tex.sampler = 0;
    tex.source = ii;
    int ti = (int)model->textures.size();
    model->textures.push_back(tex);
    return ti;
}

void wows_camo_apply_uv_transform(tinygltf::Material *mat, int /*tex_index*/,
                                   const wows_camo_uv_transform &uv, const tinygltf::Sampler */*samp*/) {
    if (!mat) return;
    std::vector<tinygltf::Value> off = { tinygltf::Value(-uv.offset_u), tinygltf::Value(-uv.offset_v) };
    std::vector<tinygltf::Value> scl = { tinygltf::Value(1.0f / uv.scale_u), tinygltf::Value(1.0f / uv.scale_v) };
    mat->extensions["KHR_texture_transform"].json_value["offset"]   = tinygltf::Value(off);
    mat->extensions["KHR_texture_transform"].json_value["scale"]    = tinygltf::Value(scl);
    mat->extensions["KHR_texture_transform"].json_value["texCoord"] = tinygltf::Value(0);
}

/* ── Resolve UV transform for a given part category ───────────────── */

static const wows_camo_uv_transform *find_uv(const wows_camo_texture &tex,
                                             wows_camo_part_category cat) {
    for (const auto &uv : tex.uv_transforms)
        if (uv.category == cat) return &uv;
    return nullptr;
}

/* ── Apply variants to one primitive mesh ─────────────────────────── */

static void apply_variants_to_meshes(tinygltf::Model *model, const char *part_name,
                                     wows_camo_part_category category,
                                     const std::vector<std::string> &camo_ids,
                                     const wows_camo_db *db,
                                     const char *game_dir, int max_sz,
                                     wows_file_provider_t file_provider) {
    if (!model || !db || camo_ids.empty()) return;
    int base_mat = -1;
    for (size_t i = 0; i < model->materials.size(); ++i) {
        const auto &m = model->materials[i];
        if (m.name == part_name || m.name == "__default_grey") { base_mat = (int)i; break; }
    }
    if (base_mat < 0) return;

    /* find the original base texture for non-tiled parts */
    std::vector<uint8_t> base_png;

    /* create variant materials (one per camo that matches this part) */
    int first_var_mat = -1;
    std::vector<int> variant_mat_indices;
    std::vector<std::string> variant_names;

    for (const std::string &cid : camo_ids) {
        const wows_camo_entry *ent = wows_camo_find_entry(db, cid.c_str());
        if (!ent) continue;

        /* check whether this part's category is covered by the texture */
        bool cat_matched = false;
        for (int i = 0; i < ent->texture.n_categories; ++i)
            if (ent->texture.categories[i] == category) { cat_matched = true; break; }
        if (!cat_matched && !ent->texture.n_categories)
            cat_matched = true; /* no part restrictions — apply to all   */
        if (!cat_matched) continue;

        /* locate the colour scheme */
        const wows_camo_color_schema *schema = wows_camo_find_color_schema(db, ent->color_schema_id);
        if (!schema && ent->color_schema_id[0]) {
            vlog("camo", "colorSchema '%s' not found for camo '%s'\n", ent->color_schema_id, cid.c_str());
            continue;
        }
        wows_camo_rgba colors[4] = { {0,0,0,1}, {0,0,0,1}, {0,0,0,1}, {0,0,0,1} };
        if (schema) memcpy(colors, schema->colors, sizeof(colors));

        /* load and bake the texture */
        std::vector<uint8_t> camo_png;
        if (ent->texture.is_tiled && ent->texture.color_mask_path[0]) {
            camo_png = load_and_bake(ent->texture.color_mask_path, colors, max_sz,
                                     file_provider, game_dir);
        } else if (!ent->texture.is_tiled && ent->texture.color_mask_path[0]) {
            /* non-tiled: load DDS directly */
            std::string p = ent->texture.color_mask_path;
            FILE *f = fopen(p.c_str(), "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (sz > 0) {
                    std::vector<uint8_t> raw((size_t)sz);
                    if (fread(raw.data(), 1, (size_t)sz, f) == (size_t)sz) {
                        int w, h;
                        std::vector<uint8_t> rgba = wows_stitch_decode_dds(raw.data(), raw.size(), &w, &h);
                        if (!rgba.empty())
                            camo_png = rgba_to_png(rgba, w, h, max_sz);
                    }
                }
                fclose(f);
            }
        }

        if (camo_png.empty()) {
            vlog("camo", "no texture baked for camo '%s' on part '%s'\n", cid.c_str(), part_name);
            continue;
        }

        /* create a clone of the base material for this variant */
        const auto &src_mat = model->materials[base_mat];
        tinygltf::Material vm;
        vm.name = src_mat.name;
        vm.doubleSided = src_mat.doubleSided;
        vm.pbrMetallicRoughness = src_mat.pbrMetallicRoughness;
        vm.normalTexture = src_mat.normalTexture;
        vm.occlusionTexture = src_mat.occlusionTexture;
        vm.emissiveTexture = src_mat.emissiveTexture;
        vm.emissiveFactor = src_mat.emissiveFactor;
        /* override base colour with camo */
        int camo_tex_idx = wows_camo_embed_png(model, camo_png);
        if (camo_tex_idx >= 0)
            vm.pbrMetallicRoughness.baseColorTexture.index = camo_tex_idx;

        /* apply UV transform if defined for this part */
        const wows_camo_uv_transform *uv = find_uv(ent->texture, category);
        if (uv)
            wows_camo_apply_uv_transform(&vm, camo_tex_idx, *uv, nullptr);

        int mi = (int)model->materials.size();
        model->materials.push_back(vm);
        variant_mat_indices.push_back(mi);
        variant_names.push_back(ent->name);
        if (first_var_mat < 0) first_var_mat = mi;
    }

    if (variant_mat_indices.empty()) return;
    if (first_var_mat < 0) return;

    /* Install first variant as the primary material for all prims in
     * meshes whose name matches this part, and set the KHR_materials_variants
     * mapping block. */
    int nvar = (int)variant_mat_indices.size();

    /* Walk meshes; for each primitive, use the first variant mat and supply
     * the mapping.  We tag each mesh node with a camo extra via an extension. */
    for (size_t ni = 0; ni < model->nodes.size(); ++ni) {
        tinygltf::Node &node = model->nodes[ni];
        if (node.mesh < 0 || (size_t)node.mesh >= model->meshes.size()) continue;
        const tinygltf::Mesh &mesh = model->meshes[node.mesh];
        bool matches = (mesh.name == part_name) || (strstr(node.name.c_str(), part_name) != nullptr);
        if (!matches) continue;

        for (auto &prim : const_cast<tinygltf::Mesh &>(mesh).primitives)
            prim.material = first_var_mat;

        /* Store variant mapping on the node as a string property for consumers
         * that can read it; true glTF KHR_materials_variants uses an extension
         * on the root scene node.  We attach the mapping as a JSON string
         * property since tinygltf does not expose the extension at mesh level. */
        std::string var_json;
        var_json.reserve(256);
        var_json += "{\"variants\":[";
        for (int i = 0; i < nvar; ++i) {
            if (i) var_json += ",";
            var_json += "\"" + variant_names[i] + "\"";
        }
        var_json += "],\"mapping\":[";
        for (int i = 0; i < nvar; ++i) {
            if (i) var_json += ",";
            var_json += std::to_string(variant_mat_indices[i]);
        }
        var_json += "]}";
        node.extras = tinygltf::Value(var_json);
        break;
    }
}

/* ── Top-level variant application ───────────────────────────────── */

int wows_camo_apply_variants(tinygltf::Model *model, const wows_camo_db *db,
                              const std::vector<std::string> &camo_ids,
                              const char *part_name,
                              wows_camo_part_category category,
                              const std::vector<uint8_t> &/*base_png*/,
                              const std::vector<uint8_t> &/*mask_png*/,
                              const char *game_dir,
                              const wows_camo_rgba /*schema*/[4],
                              int max_sz,
                              wows_file_provider_t file_provider) {
    (void)base_png;
    (void)mask_png;
    (void)schema;
    if (!model || !db || !part_name || camo_ids.empty()) return 0;
    std::string pname(part_name);
    /* strip parenthesised hardpoint suffix for name matching */
    auto lp = pname.rfind('(');
    auto rp = pname.rfind(')');
    if (lp != std::string::npos && rp != std::string::npos && rp > lp)
        pname = pname.substr(0, lp);
    /* trim trailing whitespace and underscore */
    while (!pname.empty() && (pname.back() == ' ' || pname.back() == '_'))
        pname.pop_back();

    apply_variants_to_meshes(model, pname.c_str(), category, camo_ids, db, game_dir, max_sz, file_provider);
    return (int)camo_ids.size();
}

} /* extern "C */
