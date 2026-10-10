#include <Physics/Physics.h>
#include <GameFramework/PlaySession.h>
#include <functional>
#include <Engine/Components.h>
#include <Engine/Hierarchy.h>
#include <Assets/Assets.h>
#include <Engine/SceneFile.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>
using namespace shockwave;
namespace {
int failures=0;
void check(bool v,const char* message) { if (!v) { std::fprintf(stderr,"FAIL: %s\n",message);++failures; } }
bool near(float a,float b,float tolerance=.06f) { return std::abs(a-b)<tolerance; }
Entity box(World& world,glm::vec3 position,glm::vec3 half={.5f,.5f,.5f},RigidBody::Motion motion=RigidBody::Motion::Dynamic) {
    auto e=world.create();world.add<Transform>(e,{position});Collider c;c.halfExtents=half;world.add<Collider>(e,c);
    RigidBody r;r.motion=motion;r.linearDamping=0;r.angularDamping=0;world.add<RigidBody>(e,r);return e;
}
Entity floor(World& world) { return box(world,{0,-.5f,0},{20,.5f,20},RigidBody::Motion::Static); }
void ticks(PhysicsWorld& p,int count) { for (int i=0;i<count;++i) p.step(1.0/60); }
void falling(JobSystem& jobs) {
    World w;const auto ground=floor(w);const auto body=box(w,{0,4,0});PhysicsWorld p(w,jobs);p.synchronize();
    check(p.bodyCount()==2 && p.diagnostics().empty(),"bodies synchronized");
    bool begin=false;for (int i=0;i<180;++i) { p.step(1.0/60);for (const auto& event:p.contacts()) begin|=!event.sensor && event.type==ContactEvent::Type::Begin; }
    check(near(w.get<Transform>(body)->position.y,.5f),"body lands on floor");check(begin,"collision begin event");
    check(p.addImpulse(body,{2,2,0}),"impulse accepts dynamic body");ticks(p,10);check(w.get<Transform>(body)->position.x>.1f,"impulse moves body");
    check(!p.addImpulse(ground,{1,0,0}),"static impulse rejected");
    const auto hit=p.raycast({10,5,0},{0,-3,0},10);check(hit && hit->entity==ground && near(hit->distance,5) && hit->normal.y>.9f,"raycast world distance and normal");
    check(!p.raycast({10,5,0},{0,-1,0},10,ground),"raycast ignores entity");
    check(!p.raycast({0,0,0},{0,0,0},10),"zero ray rejected");
    const auto before=p.bodyCount();w.destroy(body);p.step(1.0/60);check(p.bodyCount()==before-1,"destroy removes body");
}
void forcesAndTargets(JobSystem& jobs) {
    World w;auto settings=w.create();w.add<PhysicsSettings>(settings,{{0,0,0},1});
    auto e=box(w,{0,0,0});w.get<RigidBody>(e)->mass=2;PhysicsWorld p(w,jobs);p.synchronize();
    p.addImpulse(e,{4,0,0});p.step(1.0/60);check(near(w.get<RigidBody>(e)->linearVelocity.x,2,.001f),"mass controls impulse response");
    p.addForce(e,{120,0,0});p.step(1.0/60);check(near(w.get<RigidBody>(e)->linearVelocity.x,3,.002f),"force applied for fixed tick");
    check(p.setVelocity(e,{0,0,0}),"set velocity");p.step(1.0/60);check(near(w.get<RigidBody>(e)->linearVelocity.x,0,.001f),"velocity API persists");
    auto k=box(w,{0,3,0},{.5f,.5f,.5f},RigidBody::Motion::Kinematic);p.step(1.0/60);
    w.get<Transform>(k)->position.x=1;p.step(1.0/60);const auto ray=p.raycast({1,5,0},{0,-1,0},3);
    check(ray && ray->entity==k && near(w.get<RigidBody>(k)->linearVelocity.x,60,.01f),"kinematic target produces velocity");
    w.get<Collider>(e)->halfExtents={1,1,1};p.step(1.0/60);check(p.bodyCount()==2 && p.diagnostics().empty(),"shape edits rebuild without duplicate bodies");
    w.remove<Collider>(e);p.step(1.0/60);check(p.bodyCount()==1 && !p.diagnostics().empty(),"missing collider diagnostic");
    w.add<Collider>(e);p.step(1.0/60);check(p.bodyCount()==2 && p.diagnostics().empty(),"repair missing collider");
    w.get<RigidBody>(e)->enabled=false;p.step(1.0/60);check(p.bodyCount()==1,"disabled body removed");
    w.get<RigidBody>(e)->enabled=true;p.step(1.0/60);check(p.bodyCount()==2,"body reenabled");
}
void sensorsAndMasks(JobSystem& jobs) {
    World w;floor(w);auto e=box(w,{0,3,0});auto trigger=box(w,{0,1,0},{2,2,2},RigidBody::Motion::Static);w.get<RigidBody>(trigger)->sensor=true;
    PhysicsWorld p(w,jobs);int enter=0,end=0;bool stayAfterSleep=false;
    for (int i=0;i<300;++i) {
        p.step(1.0/60);for (const auto& event:p.contacts()) if (event.sensor && event.a==trigger && event.b==e) {
            enter+=event.type==ContactEvent::Type::Begin;end+=event.type==ContactEvent::Type::End;stayAfterSleep|=i>240 && event.type==ContactEvent::Type::Stay;
        }
    }
    check(p.activeBodyCount()==0,"test target actually sleeps");
    check(enter==1 && end==0 && stayAfterSleep,"trigger overlap survives body sleeping without false exit");
    check(near(w.get<Transform>(e)->position.y,.5f),"sensor doesn't block fall");
    w.get<Transform>(e)->position.x=5;p.step(1.0/60);bool exited=false;for (const auto& event:p.contacts()) exited|=event.sensor && event.type==ContactEvent::Type::End && event.a==trigger && event.b==e;
    check(exited,"trigger exit on teleport");
    const auto ray=p.raycast({0,5,0},{0,-1,0},10,kNullEntity,true);check(ray && ray->entity==trigger,"ray optionally includes sensors");
    w.get<RigidBody>(e)->collisionLayer=1;w.get<RigidBody>(e)->collisionMask=0;w.get<Transform>(e)->position={0,2,0};ticks(p,90);
    check(w.get<Transform>(e)->position.y<0,"collision mask excludes floor");
    check(!p.raycast({0,5,0},{0,-1,0},1,kNullEntity,false,1<<1),"ray collision mask");
}
void hierarchyAndValidation(JobSystem& jobs) {
    World w;floor(w);auto parent=w.create();w.add<Transform>(parent,{{3,0,0},{1,0,0,0},{2,2,2}});
    auto child=box(w,{0,2,0});setParent(w,child,parent,false);PhysicsWorld p(w,jobs);ticks(p,180);
    check(near(worldMatrix(w,child)[3].y,1) && near(w.get<Transform>(child)->position.y,.5f),"world physics writes parent-relative transforms");
    w.get<Transform>(parent)->scale={2,1,2};p.step(1.0/60);check(p.bodyCount()==1 && !p.diagnostics().empty(),"nonuniform dynamic parent rejected clearly");
    w.get<Transform>(parent)->scale={2,2,2};p.step(1.0/60);check(p.bodyCount()==2 && p.diagnostics().empty(),"parent scale repair recovers");
    auto noTransform=w.create();w.add<Collider>(noTransform);p.step(1.0/60);check(!p.diagnostics().empty(),"missing transform diagnosed");
    w.add<Transform>(noTransform,{{5,0,0}});p.step(1.0/60);check(p.diagnostics().empty(),"adding missing transform recovers");
    auto sphere=box(w,{7,3,0});w.get<Collider>(sphere)->shape=Collider::Shape::Sphere;w.get<Transform>(sphere)->scale={1,2,1};p.step(1.0/60);
    check(!p.diagnostics().empty(),"nonuniform sphere rejected");
    w.get<Transform>(sphere)->scale={1,1,1};p.step(1.0/60);check(p.diagnostics().empty(),"sphere scale repair");
    w.get<Collider>(sphere)->radius=-1;p.step(1.0/60);check(!p.diagnostics().empty(),"negative radius rejected without Jolt assertion");
    w.get<Collider>(sphere)->radius=.5f;p.step(1.0/60);check(p.diagnostics().empty(),"radius repair");
    try { p.step(0);check(false,"zero dt should throw"); } catch (const std::invalid_argument&) {}
    try { p.step(std::numeric_limits<double>::quiet_NaN());check(false,"NaN dt should throw"); } catch (const std::invalid_argument&) {}
}
void characters(JobSystem& jobs) {
    World w;floor(w);auto e=w.create();w.add<Transform>(e,{{0,2,0}});w.add<CharacterController>(e);PhysicsWorld p(w,jobs);ticks(p,120);
    auto* c=w.get<CharacterController>(e);check(c->grounded && std::abs(w.get<Transform>(e)->position.y)<.1f,"character lands with feet at ground");
    c->desiredVelocity={2,0,0};ticks(p,60);check(w.get<Transform>(e)->position.x>1.8f,"character desired movement");
    c->desiredVelocity={0,0,0};c->jumpRequested=true;float maximum=0;for (int i=0;i<90;++i) { p.step(1.0/60);maximum=std::max(maximum,w.get<Transform>(e)->position.y); }
    check(maximum>.7f && c->grounded && !c->jumpRequested,"character jumps and lands");
    const auto ray=p.raycast({2,3,0},{0,-1,0},4);check(ray && ray->entity==e,"character inner body participates in raycasts");
    // Stair height is below the controller's configured step height.
    box(w,{3,.1f,0},{.3f,.1f,1},RigidBody::Motion::Static);c->desiredVelocity={2,0,0};maximum=0;
    for (int i=0;i<90;++i) { p.step(1.0/60);maximum=std::max(maximum,w.get<Transform>(e)->position.y); }
    check(w.get<Transform>(e)->position.x>4.5f && maximum>.15f,"character climbs low stair");
    w.remove<CharacterController>(e);p.step(1.0/60);check(p.characterCount()==0 && p.bodyCount()==2,"character cleanup removes proxy body");
}
void models(JobSystem& jobs,const std::filesystem::path& root) {
    Assets assets;auto asset=std::make_shared<ModelAsset>();std::string error;
    check(loadModel(root/"Games/Sample/Content/models/material-demo.glb",asset->data,error),error.c_str());if (asset->data.meshes.empty()) return;
    auto handle=assets.addModel("model",asset);
    World w;floor(w);auto e=box(w,{0,3,0});w.add<ModelRenderer>(e,{handle});w.get<Collider>(e)->shape=Collider::Shape::ConvexModel;
    PhysicsWorld p(w,jobs,&assets);ticks(p,180);check(p.diagnostics().empty() && near(w.get<Transform>(e)->position.y,0,.1f),"imported convex hull rests on floor");
    w.get<Collider>(e)->shape=Collider::Shape::TriangleModel;p.step(1.0/60);check(!p.diagnostics().empty(),"moving triangle mesh rejected");
    w.get<RigidBody>(e)->motion=RigidBody::Motion::Static;w.get<Transform>(e)->position={0,0,0};p.step(1.0/60);check(p.diagnostics().empty(),"static imported triangle collider");
    const auto ray=p.raycast({1.3f,3,0},{0,-1,0},4);check(ray && ray->entity==e && near(ray->point.y,1) && ray->normal.y>.9f,"mirrored imported mesh winding preserved");
    auto replacement=std::make_shared<ModelAsset>(*asset);assets.addModel("model",replacement);p.step(1.0/60);check(p.bodyCount()==2 && p.diagnostics().empty(),"model reload rebuilds collision geometry");
}
struct Lifecycle final : GameMode {
    int& steps;
    explicit Lifecycle(int& s) : steps(s) {}
    void fixedUpdate(GameContext& ctx, double) override {
        check(ctx.subsystem<PhysicsWorld>() == &ctx.physics(), "physics is a session subsystem");
        ++steps;
    }
};
// A GameInstance that makes GameModes with a lambda, for tests.
struct TestInstance final : GameInstance {
    std::function<std::unique_ptr<GameMode>()> make;
    explicit TestInstance(std::function<std::unique_ptr<GameMode>()> f) : make(std::move(f)) {}
    std::unique_ptr<GameMode> createGameMode(const World&) override { return make(); }
};

void sessionsAndFiles(JobSystem& jobs) {
    World w;floor(w);const auto body=box(w,{0,4,0});int steps=0;
    TestInstance instance([&] { return std::make_unique<Lifecycle>(steps); });
    PlaySession session(w, jobs, instance);
    session.start();auto* physics=session.physics();
    for (int i=0;i<30;++i) session.update(1.0/60);check(w.get<Transform>(body)->position.y<4 && steps==30,"PlaySession advances physics");
    session.pause();const float height=w.get<Transform>(body)->position.y;session.update(1.0/60);check(w.get<Transform>(body)->position.y==height,"paused physics freezes");
    session.start();check(session.physics()==physics,"resume keeps physics state");session.update(1.0/60);session.stop();
    check(!session.physics() && w.get<Transform>(body)->position.y==4 && w.get<RigidBody>(body)->linearVelocity.y==0,"stop restores authored transforms and velocities");
    session.start();check(session.physics()->bodyCount()==2,"restart recreates exactly two bodies");session.stop();
    const auto file=std::filesystem::temp_directory_path()/("shockwave-physics-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".swscene");
    Assets assets;std::string error;check(saveScene(w,assets,file.string(),error),error.c_str());World loaded;check(loadScene(loaded,assets,file.string(),error),error.c_str());
    check(loaded.count<Collider>()==2 && loaded.count<RigidBody>()==2,"physics components serialize");
    check(loaded.get<RigidBody>(loaded.entityAt(0))->motion==RigidBody::Motion::Static,"motion enum round trip");
    std::ofstream(file)<<"shockwave_scene 2\nentity \"invalid\"\n  Collider\n    shape 99\nend\n";
    check(!loadScene(loaded,assets,file.string(),error),"invalid enum rejected by scene loader");std::filesystem::remove(file);
}
}
int main(int argc,char** argv) {
    if (argc<2) return 2;JobSystem jobs(4);
    try { falling(jobs);forcesAndTargets(jobs);sensorsAndMasks(jobs);hierarchyAndValidation(jobs);characters(jobs);models(jobs,argv[1]);sessionsAndFiles(jobs); }
    catch (const std::exception& e) { check(false,e.what()); }
    std::printf("Physics checks: %s\n",failures ? "FAILED" : "passed");return failures ? 1 : 0;
}
