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
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>

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
        physics_system->Init(8192, 0, 8192, 8192, *bp_layer_interface, *obj_vs_bp_filter, *obj_layer_pair_filter);
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

    uint32_t phys_create_static_box(float px, float py, float pz, float hx, float hy, float hz, float rot_yaw, float rot_pitch) {
        BodyInterface& bi = physics_system->GetBodyInterface();
        Quat q_yaw = Quat::sRotation(Vec3::sAxisY(), rot_yaw);
        Quat q_pitch = Quat::sRotation(Vec3::sAxisX(), rot_pitch);
        Quat rot = q_yaw * q_pitch;

        BodyCreationSettings bcs(new BoxShapeSettings(Vec3(hx, hy, hz)), RVec3(px, py, pz), rot, EMotionType::Static, Layers::NON_MOVING);
        Body* box = bi.CreateBody(bcs);
        bi.AddBody(box->GetID(), EActivation::DontActivate);
        return box->GetID().GetIndexAndSequenceNumber();
    }

    void phys_destroy_body(uint32_t id) {
        BodyInterface& bi = physics_system->GetBodyInterface();
        BodyID bid(id);
        bi.RemoveBody(bid);
        bi.DestroyBody(bid);
    }

    uint32_t phys_create_kinematic_box(float px, float py, float pz, float hx, float hy, float hz) {
        BodyInterface& bi = physics_system->GetBodyInterface();
        BodyCreationSettings bcs(new BoxShapeSettings(Vec3(hx, hy, hz)), RVec3(px, py, pz), Quat::sIdentity(), EMotionType::Kinematic, Layers::MOVING);
        Body* box = bi.CreateBody(bcs);
        bi.AddBody(box->GetID(), EActivation::Activate);
        return box->GetID().GetIndexAndSequenceNumber();
    }

    void phys_move_kinematic(uint32_t id, float x, float y, float z) {
        BodyInterface& bi = physics_system->GetBodyInterface();
        bi.MoveKinematic(BodyID(id), RVec3(x, y, z), Quat::sIdentity(), 1.0f / 60.0f);
    }

    void phys_create_character(float px, float py, float pz) {
        float half_height = 0.5f;
        RotatedTranslatedShapeSettings capsule_settings(
            Vec3(0.0f, half_height + 0.4f, 0.0f), Quat::sIdentity(),
            new CapsuleShapeSettings(half_height, 0.4f)
        );
        ShapeRefC capsule = capsule_settings.Create().Get();

        CharacterVirtualSettings settings;
        settings.mShape = capsule;
        settings.mSupportingVolume = Plane(Vec3::sAxisY(), 0.0f);

        character = new CharacterVirtual(&settings, RVec3(px, py, pz), Quat::sIdentity(), physics_system);
    }

    void phys_teleport_character(float px, float py, float pz) {
        character->SetPosition(RVec3(px, py, pz));
    }

    void phys_refresh_contacts() {
        if (!character) return;
        DefaultBroadPhaseLayerFilter bp_filter = physics_system->GetDefaultBroadPhaseLayerFilter(Layers::MOVING);
        DefaultObjectLayerFilter obj_filter = physics_system->GetDefaultLayerFilter(Layers::MOVING);
        BodyFilter body_filter;
        ShapeFilter shape_filter;
        character->RefreshContacts(bp_filter, obj_filter, body_filter, shape_filter, *temp_allocator);
    }

    bool phys_resize_character(bool agachado) {
        float half_height = agachado ? 0.15f : 0.5f;
        RotatedTranslatedShapeSettings new_capsule_settings(
            Vec3(0.0f, half_height + 0.4f, 0.0f), Quat::sIdentity(),
            new CapsuleShapeSettings(half_height, 0.4f)
        );
        ShapeRefC new_shape = new_capsule_settings.Create().Get();

        bool success = character->SetShape(new_shape,
            1.5f * physics_system->GetPhysicsSettings().mPenetrationSlop,
            physics_system->GetDefaultBroadPhaseLayerFilter(Layers::MOVING),
            physics_system->GetDefaultLayerFilter(Layers::MOVING),
            {}, {}, *temp_allocator);

        return success;
    }

    float phys_camera_ray(float fx, float fy, float fz, float dx, float dy, float dz, float max_dist) {
        const NarrowPhaseQuery& npq = physics_system->GetNarrowPhaseQuery(); // CORREÇÃO: const devolvido
        RRayCast ray{ RVec3(fx, fy, fz), Vec3(dx, dy, dz) * max_dist };
        RayCastResult hit;

        // M1-6b: Filtros removidos. Aceita default (tudo que for sólido interage com o raio visual).
        if (npq.CastRay(ray, hit)) {
            return hit.mFraction;
        }
        return 1.0f;
    }

    PhysState phys_update_character(const PhysInput& input) {
        const float dt = 1.0f / 60.0f;
        Vec3 gravity(0.0f, -input.gravity_y, 0.0f);

        character->SetLinearVelocity(Vec3(input.vx, input.vy, input.vz));

        CharacterVirtual::ExtendedUpdateSettings settings;
        settings.mStickToFloorStepDown = Vec3(0.0f, -input.stick_step_down, 0.0f);
        settings.mWalkStairsStepUp = input.walk_stairs_on ? Vec3(0.0f, 0.4f, 0.0f) : Vec3::sZero();

        DefaultBroadPhaseLayerFilter bp_filter = physics_system->GetDefaultBroadPhaseLayerFilter(Layers::MOVING);
        DefaultObjectLayerFilter obj_filter = physics_system->GetDefaultLayerFilter(Layers::MOVING);
        BodyFilter body_filter;
        ShapeFilter shape_filter;

        character->ExtendedUpdate(dt, gravity, settings, bp_filter, obj_filter, body_filter, shape_filter, *temp_allocator);
        physics_system->Update(dt, 1, temp_allocator, job_system);

        CharacterVirtual::EGroundState ground_state = character->GetGroundState();
        bool on_ground = (ground_state == CharacterVirtual::EGroundState::OnGround);

        RVec3 pos = character->GetPosition();
        Vec3 vel = character->GetLinearVelocity();
        Vec3 g_vel = character->GetGroundVelocity();

        PhysState state;
        state.x = pos.GetX();
        state.y = pos.GetY();
        state.z = pos.GetZ();
        state.vx = vel.GetX();
        state.vy = vel.GetY();
        state.vz = vel.GetZ();
        state.gvx = g_vel.GetX();
        state.gvy = g_vel.GetY();
        state.gvz = g_vel.GetZ();
        state.on_ground = on_ground;

        return state;
    }
}