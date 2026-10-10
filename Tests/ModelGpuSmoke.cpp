// Opt-in integration test: requires a desktop and Vulkan GPU, so not registered
// with CTest. Exercises actual uploads, dependency reload, rollback and rendering.
#include <Engine/Engine.h>
#include <Assets/AssetFolder.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>
using namespace shockwave;
namespace fs=std::filesystem;
namespace {
void require(bool condition,const char* message) { if (!condition) throw std::runtime_error(message); }
struct Temporary {
    fs::path root=fs::temp_directory_path()/("shockwave-gpu-model-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temporary() { fs::create_directories(root); }
    ~Temporary() { std::error_code ec;fs::remove_all(root,ec); }
};
class Smoke final:public Game {
public:
    explicit Smoke(fs::path source):m_source(std::move(source)) {}
    void onStart() override {
        for (const auto& file:fs::directory_iterator(m_source/"Tests/fixtures/models"))
            if (file.path().extension()!=".py") fs::copy_file(file.path(),temp.root/file.path().filename());
        fs::copy_file(m_source/"assets/models/material-demo.glb",temp.root/"demo.glb");
        folder=std::make_unique<AssetFolder>(engine().renderer(),engine().assets(),engine().jobs());
        for (const auto& change:folder->open(temp.root)) require(change.what!=AssetFolder::Change::What::Failed,change.error.c_str());
        const auto h=engine().assets().findModel("parts.gltf");
        require(h.valid(),"glTF not registered");
        auto original=engine().assets().model(h);
        fs::remove(temp.root/"parts.bin");folder->refresh();
        require(!folder->find("parts.gltf")->error.empty(),"removed bin didn't trigger failure");
        require(engine().assets().model(h)==original,"failed reload replaced last good model");
        fs::copy_file(m_source/"Tests/fixtures/models/parts.bin",temp.root/"parts.bin");folder->refresh();
        require(folder->find("parts.gltf")->error.empty() && engine().assets().model(h)!=original,"repaired buffer didn't reload");
        original=engine().assets().model(h);
        fs::remove(temp.root/"checker.png");folder->refresh();
        require(engine().assets().model(h)==original,"missing texture published partial model");
        fs::copy_file(m_source/"Tests/fixtures/models/checker.png",temp.root/"checker.png");folder->refresh();
        require(folder->find("parts.gltf")->error.empty(),"texture repair didn't reload");
        const auto obj=engine().assets().findModel("quad.obj");
        auto oldObj=engine().assets().model(obj);
        std::ofstream(temp.root/"quad.mtl")<<"newmtl Paint\nKd 0.8 0.2 0.1\nmap_Kd checker.png\n";
        folder->refresh();
        require(engine().assets().model(obj)!=oldObj,"MTL save didn't reload OBJ");
        oldObj=engine().assets().model(obj);
        bool paint=false;for (const auto& m:oldObj->data.materials) paint|=std::abs(m.baseColor.r-.8f)<.0001f;
        require(paint,"reloaded OBJ material color incorrect");
        for (int i=0;i<12;++i) {
            for (const auto& c:folder->refresh(true)) require(c.what!=AssetFolder::Change::What::Failed,c.error.c_str());
            for (auto handle:engine().assets().model(h)->meshes) require(handle.id<40,"model reload leaked mesh slots");
            for (auto handle:engine().assets().model(h)->textures) require(handle.id<20,"model reload leaked texture slots");
        }
        oldObj=engine().assets().model(obj);
        fs::remove(temp.root/"quad.obj");folder->refresh();require(engine().assets().model(obj)==oldObj,"removed asset lost last good model");
        const auto e=world.create();world.add<Transform>(e);world.add<ModelRenderer>(e,{engine().assets().findModel("demo.glb")});
        std::puts("Model GPU smoke: dependencies, rollback, stable references, and 12 reloads passed");
    }
    void onRender(Renderer& r,double) override {
        r.setClearColor({.03f,.04f,.06f,1});
        r.setCamera(glm::lookAt(glm::vec3(4,3,6),glm::vec3(0,.5f,0),glm::vec3(0,1,0)),perspectiveReverseZ(glm::radians(50.0f),r.aspectRatio(),.1f));
        renderWorld(world,r,engine().jobs(),1,&engine().assets());
    }
    void onFrame(double) override { if (++frames==3) folder->refresh(true); }
    void onShutdown() override { require(engine().renderer().drawCount()==3,"model parts not rendered");std::puts("Model GPU smoke: three material parts rendered"); }
private:
    fs::path m_source;
    Temporary temp;
    World world;
    std::unique_ptr<AssetFolder> folder;
    int frames=0;
};
}
int main(int argc,char** argv) {
    try {
        if (argc<2) return 2;
        EngineConfig config;config.window={"Shockwave Model Smoke",960,600,false};config.maxFrames=12;config.workerThreads=4;config.showFpsInTitle=false;
        Engine engine(config);Smoke game(fs::absolute(argv[1]));engine.run(game);
    } catch (const std::exception& e) { std::fprintf(stderr,"Model GPU smoke: %s\n",e.what());return 1; }
}
