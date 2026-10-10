#include <shockwave/Model.h>
#include <shockwave/Assets.h>
#include <shockwave/Components.h>
#include <shockwave/SceneFile.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <cmath>
using namespace shockwave;
namespace fs=std::filesystem;
namespace {
int failures=0;
void check(bool value,const char* message) { if (!value) { std::fprintf(stderr,"FAIL: %s\n",message); ++failures; } }
bool near(float a,float b) { return std::abs(a-b)<.0001f; }
void imported(const fs::path& root,const char* filename) {
    ModelData m; std::string error;
    const bool ok=loadModel(root/filename,m,error);
    check(ok,error.c_str()); if (!ok) return;
    check(m.meshes.size()==3,"all material parts preserved");
    check(m.materials.size()>=3,"material count");
    check(m.textures.size()==1 && m.textures[0].image.width==2,"external/embedded PNG decoded");
    check(m.triangles==3,"triangle count");
    check(near(m.bounds.min.x,2) && near(m.bounds.max.x,5) && near(m.bounds.max.z,1),"nested transforms and bounds");
    bool mask=false,blend=false;
    for (const auto& mat:m.materials) {
        mask |= mat.alpha==ModelMaterial::Alpha::Mask && mat.doubleSided && near(mat.alphaCutoff,.4f);
        blend |= mat.alpha==ModelMaterial::Alpha::Blend && near(mat.baseColor.a,.4f);
    }
    check(mask && blend,"mask/blend properties");
    float t=0;
    check(intersectModel(m,{2.2f,.2f,2},{0,0,-1},t) && near(t,1),"nearest triangle in transformed child");
    check(!intersectModel(m,{2.9f,.9f,2},{0,0,-1},t),"triangle picking rejects empty part of bounds");
    check(m.nodes.size()>=4 && m.nodes[1].parent==0,"parent-first hierarchy");
    int instances=0;for (const auto& node:m.nodes) for (auto mesh:node.meshes) instances+=mesh==0;
    check(instances==2 && near(m.bounds.max.y,3),"repeated mesh instances preserved");
    check(near(m.meshes[0].data.vertices[0].uv.y,0),"glTF texture orientation");
}
void scenes(const fs::path& file) {
    Assets assets;
    auto first=std::make_shared<ModelAsset>();
    const auto handle=assets.addModel("models/parts with spaces.glb",first);
    World world; const auto e=world.create();world.add<Name>(e,{"Imported"});world.add<Transform>(e);world.add<ModelRenderer>(e,{handle,{.1f,.2f,.3f,1},false});
    std::string error;check(saveScene(world,assets,file.string(),error),error.c_str());
    World loaded;std::vector<std::string> warnings;
    check(loadScene(loaded,assets,file.string(),error,{},&warnings),error.c_str());
    check(warnings.empty() && loaded.count<ModelRenderer>()==1,"model reference round trip");
    loaded.each<const ModelRenderer>([&](const auto& r) { check(r.model.id==handle.id && !r.visible && near(r.tint.z,.3f),"model inspector fields serialized"); });
    auto replacement=std::make_shared<ModelAsset>();
    check(assets.addModel("models/parts with spaces.glb",replacement).id==handle.id,"reload keeps stable model handle");
    check(assets.model(handle)==replacement && first!=replacement,"atomic asset replacement");
    Assets empty;
    warnings.clear();check(loadScene(loaded,empty,file.string(),error,{},&warnings),error.c_str());
    check(warnings.size()==1,"missing model produces scene warning");
}
}
int main(int argc,char** argv) {
    if (argc<2) return 2;
    const fs::path root=fs::path(argv[1])/"tests/fixtures/models";
    check(isModelFile("THING.GLTF") && isModelFile("a.glb") && isModelFile("b.fbx") && isModelFile("c.blend") && isModelFile("d.obj"),"format detection, case insensitive");
    check(!isModelFile("a.bin") && !isModelFile("a.png") && !isModelFile("a.mtl"),"dependencies are not model assets");
    imported(root,"parts.gltf"); imported(root,"parts.glb");
    ModelData quad;std::string error;
    check(loadModel(root/"quad.obj",quad,error),error.c_str());
    check(quad.triangles==2 && quad.textures.size()==1,"OBJ quad triangulation and MTL texture");
    bool mtl=false;for (const auto& p:quad.dependencies) mtl |= p.filename()=="quad.mtl";
    check(mtl,"MTL dependency tracking");
    const auto temporary=fs::temp_directory_path()/("shockwave-model-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(temporary);
    fs::copy_file(root/"parts.gltf",temporary/"broken.gltf");
    ModelData unchanged=quad;std::vector<fs::path> attempted;
    check(!loadModel(temporary/"broken.gltf",unchanged,error,&attempted) && !error.empty(),"missing external buffer fails clearly");
    check(unchanged.triangles==quad.triangles,"failed import preserves destination");
    bool bin=false;for (const auto& p:attempted) bin |= p.filename()=="parts.bin";
    check(bin,"failed dependency retained for retry");
    fs::copy_file(root/"parts.bin",temporary/"parts.bin");
    check(!loadModel(temporary/"broken.gltf",unchanged,error,&attempted),"missing texture fails without publishing partial model");
    bool png=false;for (const auto& p:attempted) png |= p.filename()=="checker.png";
    check(png,"missing texture tracked");
    fs::copy_file(root/"checker.png",temporary/"checker.png");
    check(loadModel(temporary/"broken.gltf",unchanged,error),"repaired dependencies import successfully");
    scenes(temporary/"model.swscene");
    // Additional upstream format probes are opt-in and never needed by CTest.
    for (int i=2;i<argc;++i) { ModelData probe;const bool ok=loadModel(argv[i],probe,error);check(ok,error.c_str()); if (ok) std::printf("Imported %s: %zu meshes, %llu triangles\n",argv[i],probe.meshes.size(),static_cast<unsigned long long>(probe.triangles)); }
    fs::remove_all(temporary);
    return failures ? 1 : 0;
}
