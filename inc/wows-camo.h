/**
 * @file wows-camo.h
 * @brief XML-driven camouflage material variant support for glTF exports.
 *
 * Parses the game's `camouflages.xml`, resolves camouflage entries for a given
 * ship from `GameParams.data`, classifies model parts, bakes tiled colour-mask
 * DDS textures with the selected colour scheme, and emits selectable glTF
 * variants via `KHR_materials_variants` with per-part UV transforms via
 * `KHR_texture_transform`.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <functional>
#include "wows-model-exporter.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaque handle ──────────────────────────────────────────────────── */

typedef struct wows_camo_db wows_camo_db;

/* ── Data structures ────────────────────────────────────────────────── */

/**
 * @brief Four-component colour used in a camouflage colour scheme.
 */
typedef struct {
    float r, g, b, a;
} wows_camo_rgba;

/**
 * @brief A colour scheme grouping four RGBA colours under a named ID.
 */
typedef struct {
    char id[64];
    char name[128];
    wows_camo_rgba colors[4];
} wows_camo_color_schema;

/**
 * @brief Part-category classification for a single texture entry.
 */
typedef enum {
    WOWS_CAMO_PART_HULL_TILE  = 0,
    WOWS_CAMO_PART_DECKHOUSE  = 1,
    WOWS_CAMO_PART_GUN        = 2,
    WOWS_CAMO_PART_DIRECTOR   = 3,
    WOWS_CAMO_PART_MISC       = 4,
    WOWS_CAMO_PART_BULGE      = 5,
    WOWS_CAMO_PART_COUNT
} wows_camo_part_category;

/**
 * @brief Per-part UV transform entry inside a camouflage texture block.
 */
typedef struct {
    wows_camo_part_category category;
    float offset_u;   /* horizontal UV offset  */
    float offset_v;   /* vertical UV offset    */
    float scale_u;    /* horizontal UV scale   */
    float scale_v;    /* vertical UV scale     */
} wows_camo_uv_transform;

/**
 * @brief A texture entry within a camouflage definition.
 */
typedef struct {
    char color_mask_path[512];            /* absolute or game-relative path to mask DDS */
    bool is_tiled;                        /* true if the texture is tiled/repeatable   */
    wows_camo_part_category categories[8];/* part categories this texture applies to   */
    int   n_categories;
    wows_camo_uv_transform uv_transforms[8]; /* UV transforms keyed by part category    */
    int   n_uv_transforms;
} wows_camo_texture;

/**
 * @brief A single camouflage variant definition.
 */
typedef struct {
    char id[64];              /* unique camouflage identifier (e.g. "DL01") */
    char name[128];           /* display name ("Dazzle")                      */
    char color_schema_id[64]; /* references a wows_camo_color_schema by id    */
    bool is_tiled;            /* true if tiled/mask-based camouflage          */
    wows_camo_texture texture;
} wows_camo_entry;

/**
 * @brief Associates a ship group name with the camouflage IDs it can use.
 */
typedef struct {
    char group_name[64];
    char camo_ids[32][64];    /* up to 32 camouflage IDs per group */
    int  n_camo_ids;
} wows_camo_ship_group;

/* ── Database lifecycle ─────────────────────────────────────────────── */

/**
 * @brief Parse a camouflages.xml file into an in-memory database.
 *
 * The caller must eventually free the database with wows_camo_db_free().
 *
 * @param xml_path  Filesystem path to the XML file.
 * @return Opaque pointer on success, NULL on failure.
 */
wows_camo_db *wows_camo_open(const char *xml_path);

/**
 * @brief Parse camouflages.xml from a memory buffer.
 *
 * @param data   Pointer to XML data in memory.
 * @param size   Size of the buffer in bytes.
 * @return Opaque pointer on success, NULL on failure.
 */
wows_camo_db *wows_camo_open_memory(const uint8_t *data, size_t size);

/**
 * @brief Free all memory held by a camouflage database.
 *
 * @param db  Database to free (no-op if NULL).
 */
void wows_camo_db_free(wows_camo_db *db);

/* ── Database queries ───────────────────────────────────────────────── */

/**
 * @brief Return the number of colour schemes defined in the database.
 */
int wows_camo_color_schema_count(const wows_camo_db *db);

/**
 * @brief Return a pointer to the i-th colour schema.
 *
 * The pointer is valid for the lifetime of @p db.
 */
const wows_camo_color_schema *wows_camo_color_schema_at(const wows_camo_db *db, int i);

/**
 * @brief Look up a colour schema by its ID string.
 *
 * @return Pointer to schema, or NULL if not found.
 */
const wows_camo_color_schema *wows_camo_find_color_schema(const wows_camo_db *db, const char *id);

/**
 * @brief Return the number of camouflage entries.
 */
int wows_camo_entry_count(const wows_camo_db *db);

/**
 * @brief Return a pointer to the i-th camouflage entry.
 */
const wows_camo_entry *wows_camo_entry_at(const wows_camo_db *db, int i);

/**
 * @brief Look up a camouflage by its ID string.
 */
const wows_camo_entry *wows_camo_find_entry(const wows_camo_db *db, const char *id);

/**
 * @brief Return the number of ship groups.
 */
int wows_camo_ship_group_count(const wows_camo_db *db);

/**
 * @brief Return a pointer to the i-th ship group.
 */
const wows_camo_ship_group *wows_camo_ship_group_at(const wows_camo_db *db, int i);

/**
 * @brief Look up a ship group by name.
 */
const wows_camo_ship_group *wows_camo_find_ship_group(const wows_camo_db *db, const char *name);

/**
 * @brief Resolve the camouflage IDs available to a ship.
 *
 * Uses the ship's type/group name to look up the relevant ship group.
 *
 * @param db          Loaded camouflage database.
 * @param ship_group  Ship group name from GameParams (e.g. "Destroyer").
 * @param out_ids     Output vector populated with camouflage ID strings.
 */
void wows_camo_resolve_for_group(const wows_camo_db *db, const char *ship_group,
                                 std::vector<std::string> *out_ids);

/* ── Part classification ────────────────────────────────────────────── */

/**
 * @brief Classify an MFM stem (texture name without extension) into a
 *        part-category enum value.
 *
 * Classification rules (ordered by priority):
 *   - Hull tiled textures  : stem contains "_tile" or ends with "_hull"
 *   - Deckhouse            : stem contains "deck" or starts with "dc_"
 *   - Gun                  : stem contains "gun" or starts with "gun_"
 *   - Director             : stem contains "director" or starts with "dir_"
 *   - Bulge                : stem contains "bulge" or starts with "blg_"
 *   - Misc                 : anything else
 *
 * @param mfm_stem  Texture stem string (lowercased).
 * @param category  Output category enum value.
 */
void wows_camo_classify_part(const char *mfm_stem, wows_camo_part_category *category);

/* ── Texture baking ─────────────────────────────────────────────────── */

/**
 * @brief Bake a tiled camouflage albedo texture by modulating a mask DDS with
 *        a colour scheme.
 *
 * The mask DDS is decoded, then each texel's RGB channels are multiplied by
 * the 4 chosen colour-scheme colours (via palette / quantise step).  The
 * result is re-encoded as PNG bytes suitable for embedding in a glTF.
 *
 * @param mask_data   Pointer to raw DDS bytes.
 * @param mask_size   Size of the DDS data in bytes.
 * @param schema      Four-element colour-scheme array.
 * @param max_sz      Maximum output dimension (0 = no limit).
 * @return PNG-encoded bytes, empty on failure.
 */
std::vector<uint8_t> wows_camo_bake_tiled(const uint8_t *mask_data, size_t mask_size,
                                           const wows_camo_rgba schema[4], int max_sz);

/**
 * @brief Bake from a filesystem path.
 */
std::vector<uint8_t> wows_camo_bake_tiled_file(const char *mask_path, const wows_camo_rgba schema[4],
                                                int max_sz);

/* ── glTF extension helpers ─────────────────────────────────────────── */

/**
 * @brief Embed a PNG image into a glTF model and return the texture index.
 */
int wows_camo_embed_png(tinygltf::Model *model, const std::vector<uint8_t> &png);

/**
 * @brief Apply KHR_texture_transform to a texture in a material.
 */
void wows_camo_apply_uv_transform(tinygltf::Material *mat, int tex_index,
                                  const wows_camo_uv_transform &uv,
                                  const tinygltf::Sampler *samp);

/**
 * @brief Build the KHR_materials_variants extension block for a glTF model.
 *
 * Creates one material per camouflage variant that is applicable to the part,
 * and records the mapping so that viewers supporting KHR_materials_variants
 * can offer a variant selector.
 *
 * @param model      The merged glTF model to modify.
 * @param db         Loaded camouflage database.
 * @param camo_ids   Vector of camouflage ID strings to expose as variants.
 * @param part_name  Name of the mesh part being processed.
 * @param category   Part category for UV transform lookup.
 * @param base_png   Already-baked albedo PNG bytes (non-tiled); empty if none.
 * @param mask_png   Already-baked tiled+mask PNG bytes; empty if none.
 * @param game_dir   Game root directory for resolving mask DDS paths.
 * @param schema     Colour scheme to use for tiled bake.
 * @param max_sz     Max texture dimension.
 * @param file_provider Optional: file-reader callback for archive-backed files.
 * @return Number of variant materials emitted (0 = no camo for this part).
 */
int wows_camo_apply_variants(tinygltf::Model *model, const wows_camo_db *db,
                              const std::vector<std::string> &camo_ids,
                              const char *part_name,
                              wows_camo_part_category category,
                              const std::vector<uint8_t> &base_png,
                              const std::vector<uint8_t> &mask_png,
                              const char *game_dir,
                              const wows_camo_rgba schema[4],
                              int max_sz,
                              wows_file_provider_t file_provider);

#ifdef __cplusplus
}
#endif
