#include "wows-model-exporter.h"

#include <cassert>
#include <cstdint>
#include <vector>

static tinygltf::Primitive make_primitive() {
    tinygltf::Primitive prim;
    prim.mode = TINYGLTF_MODE_TRIANGLES;
    return prim;
}

static void test_mg_rgba_to_gltf_orm_transform() {
    std::vector<uint8_t> rgba = {
        10, 20, 30, 40,
        0, 255, 255, 128,
    };

    wows_stitch_convert_mg_to_orm(rgba);

    assert((rgba == std::vector<uint8_t>{
                        30, 235, 10, 255,
                        255, 0, 0, 255,
                    }));
}

static void test_purge_empty_meshes_preserves_scene_hierarchy() {
    tinygltf::Model model;

    tinygltf::Mesh empty_parent_mesh;
    empty_parent_mesh.name = "empty_parent_mesh";
    model.meshes.push_back(empty_parent_mesh);

    tinygltf::Mesh live_mesh;
    live_mesh.name = "live_mesh";
    live_mesh.primitives.push_back(make_primitive());
    model.meshes.push_back(live_mesh);

    tinygltf::Mesh empty_child_mesh;
    empty_child_mesh.name = "empty_child_mesh";
    model.meshes.push_back(empty_child_mesh);

    tinygltf::Node root;
    root.name = "root";
    root.children = {1, 2};
    model.nodes.push_back(root);

    tinygltf::Node empty_parent;
    empty_parent.name = "empty_parent";
    empty_parent.mesh = 0;
    empty_parent.children = {3};
    empty_parent.translation = {1.0, 2.0, 3.0};
    model.nodes.push_back(empty_parent);

    tinygltf::Node live_node;
    live_node.name = "live_node";
    live_node.mesh = 1;
    model.nodes.push_back(live_node);

    tinygltf::Node empty_child;
    empty_child.name = "empty_child";
    empty_child.mesh = 2;
    model.nodes.push_back(empty_child);

    wows_stitch_purge_empty_meshes(model);

    assert(model.meshes.size() == 1);
    assert(model.meshes[0].name == "live_mesh");
    assert(model.nodes[0].children == std::vector<int>({1, 2}));
    assert(model.nodes[1].mesh == -1);
    assert(model.nodes[1].children == std::vector<int>({3}));
    assert(model.nodes[1].translation == std::vector<double>({1.0, 2.0, 3.0}));
    assert(model.nodes[2].mesh == 0);
    assert(model.nodes[3].mesh == -1);
}

int main() {
    test_mg_rgba_to_gltf_orm_transform();
    test_purge_empty_meshes_preserves_scene_hierarchy();
    return 0;
}
