#include "managers/animation/Utils/CooldownManager.hpp"
#include "managers/damage/CollisionDamage.hpp"
#include "managers/damage/SizeHitEffects.hpp"
#include "managers/damage/LaunchObject.hpp"
#include "magic/effects/TinyCalamity.hpp"
#include "managers/RipClothManager.hpp"
#include "managers/ai/aifunctions.hpp"
#include "managers/GtsSizeManager.hpp"
#include "managers/CrushManager.hpp"
#include "managers/InputManager.hpp"
#include "magic/effects/common.hpp"
#include "managers/GtsManager.hpp"
#include "managers/Attributes.hpp"
#include "managers/hitmanager.hpp"
#include "managers/highheel.hpp"
#include "utils/actorUtils.hpp"
#include "data/persistent.hpp"
#include "ActionSettings.hpp"
#include "data/transient.hpp"
#include "data/runtime.hpp"
#include "scale/scale.hpp"
#include "UI/DebugAPI.hpp"
#include "data/time.hpp"
#include "profiler.hpp"
#include "Config.hpp"
#include "timer.hpp"
#include "node.hpp"
#include <vector>
#include <string>

using namespace Gts;
using namespace RE;
using namespace SKSE;
using namespace std;

namespace {
	void Break_Object(TESObjectREFR* ref, float damage, float giant_size, bool smt) {
		if (giant_size > 1.5f || smt) {
			if (smt) {
				damage *= 4.0f;
			}
			ref->DamageObject(damage * 40, true);
		}
	}

	float Multiply_By_Perk(Actor* giant) {
		float multiply = 1.0f;
		if (Runtime::HasPerkTeam(giant, "RumblingFeet")) {
			multiply *= 1.25f;
		}

		if (Runtime::HasPerkTeam(giant, "DisastrousTremor")) {
			multiply *= 1.5f;
		}
		return multiply;
	}

	float Multiply_By_Mass(bhkRigidBody* body) {
		float mass_total = 1.0f;
		if (body->referencedObject) {
			if (const auto havokRigidBody = static_cast<hkpRigidBody*>(body->referencedObject.get())) {
				hkVector4 mass_get = havokRigidBody->motion.inertiaAndMassInv;
				float mass = reinterpret_cast<float*>(&mass_get.quad)[3];
				
				if (mass > 0) {
					//log::info("Basic Mass is {}", mass);
					
					mass_total /= mass;

					//log::info("Mass of object is {}", mass_total);
					mass_total *= 0.5f; // Just to have old push force on objects
				}
			}
		}
		return mass_total;
	}

    void ApplyPhysicsToObject_Towards(Actor* giant, TESObjectREFR* object, NiPoint3 push, float force, float scale) {
		force *= GetLaunchPower_Object(scale, false); // Do not take perk into account here

		NiAVObject* Node = object->Get3D1(false);
		if (Node) {
			auto collision = Node->GetCollisionObject();
			if (collision) {
				bhkRigidBody* body = collision->GetRigidBody();
				if (body) {
					push *= Multiply_By_Mass(body);
					//log::info("Applying force to object, Push: {}, Force: {}, Result: {}", Vector2Str(push), force, Vector2Str(push * force));
					SetLinearImpulse(body, hkVector4(push.x * force, push.y * force, push.z * force, 1.0f));
				}
			}
		}
	}
}



namespace Gts {
    float GetLaunchPower_Object(float sizeRatio, bool Launch) {
		// https://www.desmos.com/calculator/wh0vwgljfl
		if (Launch) {
			SoftPotential launch {
				.k = 1.6f,//1.42f,
				.n = 0.70f,//0.78f
				.s = 0.6f,
				.a = 0.0f,
			};
			return soft_power(sizeRatio, launch);
		} else {
			SoftPotential kick {
			.k = 1.6f,//1.42f,
			.n = 0.62f,//0.78f
			.s = 0.6f,
			.a = 0.0f,
			};
			return soft_power(sizeRatio, kick);
		}
	}

    void PushObjectsUpwards(Actor* giant, std::vector<NiPoint3> footPoints, float maxFootDistance, float power, bool IsFoot) {
		auto profiler = Profilers::Profile("Other: Launch Objects");
		bool AllowLaunch = Persistent::GetSingleton().launch_objects;
		if (!AllowLaunch) {
			return;
		}

		float giantScale = get_visual_scale(giant);
		bool smt = false;

		power *= Multiply_By_Perk(giant);
		power *= GetHighHeelsBonusDamage(giant, true);
		float HH = HighHeelManager::GetHHOffset(giant).Length();

		if (HasSMT(giant)) {
			smt = true;
			power *= 1.25f;
		}

		if (giantScale < 2.5f) {  // slowly gain power of pushing
			float reduction = (giantScale - 1.5f);
			if (smt) {
				reduction = 0.6f;
			}
			if (reduction < 0.0f) {
				reduction = 0.0f;
			}
			power *= reduction;
		}

		float start_power = Push_Object_Upwards * (1.0f + Potion_GetMightBonus(giant));

		std::vector<ObjectRefHandle> Refs = GetNearbyObjects(giant);

		if (IsDebugEnabled() && (giant->formID == 0x14 || IsTeammate(giant) || EffectsForEveryone(giant))) {
			for (auto point: footPoints) {
				if (IsFoot) {
					point.z -= HH;
				}
				DebugAPI::DrawSphere(glm::vec3(point.x, point.y, point.z), maxFootDistance, 600, {0.0f, 1.0f, 0.0f, 1.0f});
			}
		}

        for (auto object: Refs) {
			if (object) {
				auto objectref = object.get().get();
				if (objectref) {
					if (objectref && objectref->Is3DLoaded()) {
						NiPoint3 objectlocation = objectref->GetPosition();
						for (auto point: footPoints) {
							if (IsFoot) {
								point.z -= HH;
							}
							float distance = (point - objectlocation).Length();
							if (distance <= maxFootDistance) {
								float force = 1.0f - distance / maxFootDistance;
								float push = start_power * GetLaunchPower_Object(giantScale, true) * force * power;
								auto Object1 = objectref->Get3D1(false);
							
								Break_Object(objectref, push, giantScale, smt);

								if (Object1) {
									auto collision = Object1->GetCollisionObject();
									if (collision) {
										auto body = collision->GetRigidBody();
										if (body) {
											push *= Multiply_By_Mass(body);
											SetLinearImpulse(body, hkVector4(0, 0, push, push));
										}
									}
								}
							}
						}
					}
				}
			}
		}
	}
    
            
    void PushObjectsTowards(Actor* giant, TESObjectREFR* object, NiAVObject* Bone, float power, float radius, bool Kick) {
		auto profiler = Profilers::Profile("Other: Launch Objects");
		bool AllowLaunch = Persistent::GetSingleton().launch_objects;
		if (!AllowLaunch) {
			return;
		}

		if (!Bone) {
			return;
		} 
		if (!object) {
			return;
		}
        if (!object->Is3DLoaded()) {
			return;
        }
        if (!object->GetCurrent3D()) {
            return;
        }

		bool smt = HasSMT(giant);
		float giantScale = get_visual_scale(giant);

		float start_power = Push_Object_Forward * (1.0f + Potion_GetMightBonus(giant));

		NiPoint3 point = Bone->world.translate;
		float maxDistance = radius * giantScale;

        if (Kick) { // Offset pos down
            float HH = HighHeelManager::GetHHOffset(giant).Length();
			point.z -= HH * 0.75f;
		}

		if (IsDebugEnabled() && (giant->formID == 0x14 || IsTeammate(giant) || EffectsForEveryone(giant))) {
			DebugAPI::DrawSphere(glm::vec3(point.x, point.y, point.z), maxDistance, 20, {0.5f, 0.0f, 0.5f, 1.0f});
		}

		int nodeCollisions = 0;

		auto model = object->Get3D1(false);
		if (model) {
			VisitNodes(model, [&nodeCollisions, point, maxDistance](NiAVObject& a_obj) {
				float distance = (point - a_obj.world.translate).Length();
				if (distance < maxDistance) {
					nodeCollisions += 1;
					return false;
				}
				return true;
			});
		}

		if (nodeCollisions > 0) {
			double Start = Time::WorldTimeElapsed();
			ActorHandle gianthandle = giant->CreateRefHandle();
			ObjectRefHandle objectref = object->CreateRefHandle();
			std::string name = std::format("PushObject_{}_{}", giant->formID, object->formID);

			NiPoint3 StartPos = Bone->world.translate;

			TaskManager::Run(name, [=](auto& progressData) {
				if (!gianthandle) {
					return false;
				} if (!objectref) {
					return false;
				}
				auto giantref = gianthandle.get().get();
				auto ref = objectref.get().get();
				double Finish = Time::WorldTimeElapsed();
				float timepassed = static_cast<float>(Finish - Start);

				if (timepassed > 1e-4f) {
					NiPoint3 EndPos = Bone->world.translate;
					ApplyPhysicsToObject_Towards(giantref, ref, EndPos - StartPos, start_power, giantScale);
					Break_Object(ref, power * giantScale * start_power, giantScale, smt);
					return false; // end it
				}
				return true;
			});
		}
	}

	void PushObjects(std::vector<ObjectRefHandle> refs, Actor* giant, NiAVObject* bone, float power, float radius, bool Kick) {
		if (!refs.empty()) {
			for (auto object: refs) {
				if (object) {
					TESObjectREFR* objectref = object.get().get();
					PushObjectsTowards(giant, objectref, bone, power, radius, Kick);
				}
			}
		}
	}

	std::vector<ObjectRefHandle> GetNearbyObjects(Actor* giant) {
		bool AllowLaunch = Persistent::GetSingleton().launch_objects;
		if (!AllowLaunch) {
			return {};
		}
		float giantScale = get_visual_scale(giant);

		float maxDistance = 220 * giantScale;

		std::vector<ObjectRefHandle> Objects = {};
		NiPoint3 point = giant->GetPosition();

		bool PreciseScan = Runtime::GetBoolOr("AccurateCellScan", false);

		if (!PreciseScan) { // Scan single cell only
			TESObjectCELL* cell = giant->GetParentCell();
			if (cell) {
				auto data = cell->GetRuntimeData();
				for (auto object: data.references) {
					if (object) {
						auto objectref = object.get();
						if (objectref) {
							bool IsActor = objectref->Is(FormType::ActorCharacter);
							if (!IsActor) { // we don't want to apply it to actors
								NiPoint3 objectlocation = objectref->GetPosition();
								float distance = (point - objectlocation).Length();
								if (distance <= maxDistance) {
									ObjectRefHandle refhandle = objectref->CreateRefHandle(); 
									Objects.push_back(refhandle);
								}
							}
						}
					}
				}
			}
		} else if (PreciseScan) { // Else scan Entire world
			TESObjectREFR* GiantRef = skyrim_cast<TESObjectREFR*>(giant);
			if (GiantRef) {
				//Will this ctd on AE? lets find out
				TES::GetSingleton()->ForEachReferenceInRange(GiantRef, maxDistance, [&](RE::TESObjectREFR* a_ref) {
					bool IsActor = a_ref->Is(FormType::ActorCharacter);
					if (!IsActor) { // we don't want to apply it to actors
						NiPoint3 objectlocation = a_ref->GetPosition();
						float distance = (point - objectlocation).Length();
						if (distance <= maxDistance) {
							ObjectRefHandle handle = a_ref->CreateRefHandle();
							if (handle) {
								Objects.push_back(handle);
							}
						}
					}
					return RE::BSContainer::ForEachResult::kContinue;    
				});
			}
		}
		
		return Objects;
	}
}