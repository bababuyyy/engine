#include "physics.h"
#include <cstdarg>
#include <cstdio>
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>

JPH_SUPPRESS_WARNINGS

namespace phys {
    using namespace JPH;

    static void TraceImpl(const char* inFMT, ...) {
        va_list list;
        va_start(list, inFMT);
        vprintf(inFMT, list);
        va_end(list);
    }
#ifdef JPH_ENABLE_ASSERTS
    static bool AssertFailedImpl(const char* inExpression, const char* inMessage, const char* inFile, uint32_t inLine) {
        return true;
    }
#endif

    namespace Layers {
        static constexpr ObjectLayer NON_MOVING = 0;
        static constexpr ObjectLayer MOVING = 1;
        static constexpr ObjectLayer NUM_LAYERS = 2;
    };

    namespace BroadPhaseLayers {
        static constexpr BroadPhaseLayer NON_MOVING(0);
        static constexpr BroadPhaseLayer MOVING(1);
        static constexpr uint32_t NUM_LAYERS(2);
    };

    class BPLayerInterfaceImpl final : public BroadPhaseLayerInterface {
    public:
        BPLayerInterfaceImpl() {
            mObjectToBroadPhase[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
            mObjectToBroadPhase[Layers::MOVING] = BroadPhaseLayers::MOVING;
        }
        virtual uint32_t GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM_LAYERS; }
        virtual BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer inLayer) const override {
            return mObjectToBroadPhase[inLayer];
        }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
        // Nome legível das camadas (o profiler da Jolt exige este override quando ativo)
        virtual const char* GetBroadPhaseLayerName(BroadPhaseLayer inLayer) const override
        {
            return ((BroadPhaseLayer::Type)inLayer == 0) ? "NON_MOVING" : "MOVING";
        }
#endif

    private:
        BroadPhaseLayer mObjectToBroadPhase[Layers::NUM_LAYERS];
    };

    class ObjectVsBroadPhaseLayerFilterImpl : public ObjectVsBroadPhaseLayerFilter {
    public:
        virtual bool ShouldCollide(ObjectLayer inLayer1, BroadPhaseLayer inLayer2) const override {
            switch (inLayer1) {
            case Layers::NON_MOVING: return inLayer2 == BroadPhaseLayers::MOVING;
            case Layers::MOVING:     return true;
            default: return false;
            }
        }
    };

    class ObjectLayerPairFilterImpl : public ObjectLayerPairFilter {
    public:
        virtual bool ShouldCollide(ObjectLayer inObject1, ObjectLayer inObject2) const override {
            switch (inObject1) {
            case Layers::NON_MOVING: return inObject2 == Layers::MOVING;
            case Layers::MOVING:     return true;
            default: return false;
            }
        }
    };

    PhysicsSystem* physics_system = nullptr;
    TempAllocatorImpl* temp_allocator = nullptr;
    JobSystemThreadPool* job_system = nullptr;

    BPLayerInterfaceImpl* bp_layer_interface = nullptr;
    ObjectVsBroadPhaseLayerFilterImpl* obj_vs_bp_filter = nullptr;
    ObjectLayerPairFilterImpl* obj_layer_pair_filter = nullptr;

    Ref<CharacterVirtual> character = nullptr;

    void phys_init() {
        JPH::Trace = TraceImpl;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = AssertFailedImpl;)

            RegisterDefaultAllocator();
        Factory::sInstance = new Factory();
        RegisterTypes();

        temp_allocator = new TempAllocatorImpl(10 * 1024 * 1024);

        job_system = new JobSystemThreadPool(cMaxPhysicsJobs, cMaxPhysicsBarriers, std::thread::hardware_concurrency() - 1);

        bp_layer_interface = new BPLayerInterfaceImpl();
        obj_vs_bp_filter = new ObjectVsBroadPhaseLayerFilterImpl();
        obj_layer_pair_filter = new ObjectLayerPairFilterImpl();

        physics_system = new PhysicsSystem();
        physics_system->Init(1024, 0, 1024, 1024, *bp_layer_interface, *obj_vs_bp_filter, *obj_layer_pair_filter);
    }

    void phys_shutdown() {
        character = nullptr;
        delete physics_system;
        delete obj_layer_pair_filter;
        delete obj_vs_bp_filter;
        delete bp_layer_interface;
        delete job_system;
        delete temp_allocator;
        delete Factory::sInstance;
        Factory::sInstance = nullptr;
    }

    void phys_create_static_box(float px, float py, float pz, float hx, float hy, float hz) {
        BodyInterface& body_interface = physics_system->GetBodyInterface();
        // ShapeSettings nasce no HEAP: a Jolt assume posse via refcount e o
        // destroi sozinha — nunca passar endereco de variavel local para ela.
        BodyCreationSettings bcs(new BoxShapeSettings(Vec3(hx, hy, hz)),
            RVec3(px, py, pz), Quat::sIdentity(),
            EMotionType::Static, Layers::NON_MOVING);
        Body* box = body_interface.CreateBody(bcs);
        body_interface.AddBody(box->GetID(), EActivation::DontActivate);
    }

    void phys_create_character(float px, float py, float pz) {
        Ref<Shape> capsule = CapsuleShapeSettings(0.5f, 0.4f).Create().Get();
        CharacterVirtualSettings settings;
        settings.mShape = capsule;

        settings.mSupportingVolume = Plane(Vec3::sAxisY(), -0.5f);

        character = new CharacterVirtual(&settings, RVec3(px, py, pz), Quat::sIdentity(), physics_system);
    }

    PhysState phys_update_character(const PhysInput& input) {
        const float dt = 1.0f / 60.0f;
        Vec3 gravity(0.0f, -20.0f, 0.0f);

        CharacterVirtual::EGroundState ground_state = character->GetGroundState();
        bool on_ground = (ground_state == CharacterVirtual::EGroundState::OnGround);

        Vec3 vel = character->GetLinearVelocity();

        if (on_ground && vel.GetY() < 0.0f) {
            vel.SetY(0.0f);
        }
        vel.SetY(vel.GetY() + gravity.GetY() * dt);

        if (input.jump && on_ground) {
            vel.SetY(8.0f);
        }

        vel.SetX(input.move_x * 6.0f);
        vel.SetZ(input.move_z * 6.0f);

        character->SetLinearVelocity(vel);

        DefaultBroadPhaseLayerFilter bp_filter = physics_system->GetDefaultBroadPhaseLayerFilter(Layers::MOVING);
        DefaultObjectLayerFilter obj_filter = physics_system->GetDefaultLayerFilter(Layers::MOVING);
        BodyFilter body_filter;
        ShapeFilter shape_filter;

        character->Update(dt, gravity, bp_filter, obj_filter, body_filter, shape_filter, *temp_allocator);

        physics_system->Update(dt, 1, temp_allocator, job_system);

        RVec3 pos = character->GetPosition();
        PhysState state;
        state.x = pos.GetX();
        state.y = pos.GetY();
        state.z = pos.GetZ();
        state.on_ground = on_ground;

        return state;
    }
}