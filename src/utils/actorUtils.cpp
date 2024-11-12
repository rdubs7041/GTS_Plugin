#include "managers/animation/Utils/CooldownManager.hpp"
#include "managers/animation/Utils/AnimationUtils.hpp"
#include "managers/animation/TinyCalamity_Shrink.hpp"
#include "managers/animation/AnimationManager.hpp"
#include "managers/gamemode/GameModeManager.hpp"
#include "managers/damage/CollisionDamage.hpp"
#include "managers/damage/TinyCalamity.hpp"
#include "managers/animation/HugShrink.hpp"
#include "managers/damage/LaunchActor.hpp"
#include "managers/ai/aifunctions.hpp"
#include "managers/GtsSizeManager.hpp"
#include "managers/animation/Grab.hpp"
#include "managers/audio/footstep.hpp"
#include "magic/effects/common.hpp"
#include "managers/Attributes.hpp"
#include "utils/papyrusUtils.hpp"
#include "managers/explosion.hpp"
#include "utils/DeathReport.hpp"
#include "managers/highheel.hpp"
#include "utils/actorUtils.hpp"
#include "colliders/actor.hpp"
#include "managers/Rumble.hpp"
#include "utils/findActor.hpp"
#include "data/persistent.hpp"
#include "ActionSettings.hpp"
#include "data/transient.hpp"
#include "utils/looting.hpp"
#include "scale/height.hpp"
#include "data/runtime.hpp"
#include "colliders/RE.hpp"
#include "rays/raycast.hpp"
#include "scale/scale.hpp"
#include "UI/DebugAPI.hpp"
#include "utils/debug.hpp"
#include "utils/av.hpp"
#include "profiler.hpp"
#include "events.hpp"
#include "spring.hpp"
#include "timer.hpp"

#include "node.hpp"

#include <vector>
#include <string>

using namespace RE;
using namespace Gts;

namespace RE {
	constexpr REL::Version RUNTIME_SSE_1_6_659(1, 6, 659, 0);
	constexpr REL::Version RUNTIME_SSE_1_6_678(1, 6, 678, 0);
	constexpr REL::Version RUNTIME_SSE_1_6_1130(1, 6, 1130, 0);
	constexpr REL::Version RUNTIME_SSE_1_6_1170(1, 6, 1170, 0);
}

namespace {
	const float EPS = 1e-4f;

	const std::vector<std::string> disarm_nodes = {
		"WeaponDagger",
		"WeaponAxe",
		"WeaponSword",
		"WeaponMace",
		"WEAPON",
		"SHIELD",
		"QUIVER",
		"WeaponBow",
		"WeaponBack",
	};

	float GetGrowthReduction(float size) {
		// https://www.desmos.com/calculator/pqgliwxzi2
		
		if (SizeManager::GetSingleton().BalancedMode() >= 2.0f) {
			SoftPotential cut {
				.k = 1.08f,
				.n = 0.90f,
				.s = 3.00f,
				.a = 0.0f,
			};
			float balance = GameModeManager::GetSingleton().GetBalanceModeInfo(BalanceModeInfo::SizeGain_Penalty);
			float power = soft_power(size, cut) * balance;
			return std::clamp(power, 1.0f, 99999.0f);
		} else {
			return 1.0f;
			// So it never reports values below 1.0f. Just to make sure.
		}
	}

	bool Utils_ManageTinyProtection(Actor* giantref, bool force_cancel, bool Balance) {
		float sp = GetAV(giantref, ActorValue::kStamina);

		if (!force_cancel && Balance) {
			float perk = Perk_GetCostReduction(giantref);
			float damage = 0.08f * TimeScale() * perk;
			if (giantref->formID != 0x14) {
				damage *= 0.5f; // less stamina drain for NPC's
			}
			DamageAV(giantref, ActorValue::kStamina, damage);
		}

		if (sp <= 1.0f || force_cancel) {
			float OldScale;
			giantref->GetGraphVariableFloat("GiantessScale", OldScale); // save old scale
			giantref->SetGraphVariableFloat("GiantessScale", 1.0f); // Needed to allow Stagger to play, else it won't work

			if (!force_cancel) {
				StaggerActor(giantref, 0.25f);
			}
			float scale = get_visual_scale(giantref);

			StaggerActor_Around(giantref, 48.0f, false);

			auto node = find_node(giantref, "NPC Root [Root]");
			Runtime::PlaySoundAtNode("Magic_BreakTinyProtection", giantref, 1.0f, 1.0f, "NPC COM [COM ]");
			
			if (node) {
				NiPoint3 position = node->world.translate;

				std::string name_com = std::format("BreakProtect_{}", giantref->formID);
				std::string name_root = std::format("BreakProtect_Root_{}", giantref->formID);

				Rumbling::Once(name_com, giantref, Rumble_Misc_FailTinyProtection, 0.20f, "NPC COM [COM ]", 0.0f);
				Rumbling::Once(name_root, giantref, Rumble_Misc_FailTinyProtection, 0.20f, "NPC Root [Root]", 0.0f);

				SpawnParticle(giantref, 6.00f, "GTS/Effects/TinyCalamity.nif", NiMatrix3(), position, scale * 3.4f, 7, nullptr); // Spawn it
			}
			giantref->SetGraphVariableFloat("GiantessScale", OldScale);

			return false;
		}
		return true;
	}

	float GetPerkBonus_OnTheEdge(Actor* giant, float amt) {
		float bonus = 1.0f;
		bool perk = Runtime::HasPerkTeam(giant, "OnTheEdge");
		if (perk) {
			float GetHP = std::clamp(GetHealthPercentage(giant) + 0.4f, 0.5f, 1.0f); // Bonus Size Gain if Actor has perk
			if (amt > 0) {
				bonus /= GetHP;
			} else if (amt < 0) {
				bonus *= GetHP;
			} // When health is < 60%, empower growth by up to 50%. Max value at 10% health.
		}
		return bonus;
	}

	float ShakeStrength(Actor* Source) {
		float Size = get_visual_scale(Source);
		float k = 0.065f;
		float n = 1.0f;
		float s = 1.12f;
		float Result = 1.0f/(pow(1.0f+pow(k*(Size),n*s),1.0f/s));
		return Result;
	}

	ExtraDataList* CreateExDataList() {
		size_t a_size;
		if (SKYRIM_REL_CONSTEXPR (REL::Module::IsAE()) && (REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_6_629)) {
			a_size = 0x20;
		} else {
			a_size = 0x18;
		}
		auto memory = RE::malloc(a_size);
		std::memset(memory, 0, a_size);
		if (SKYRIM_REL_CONSTEXPR (REL::Module::IsAE()) && (REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_6_629)) {
			// reinterpret_cast<std::uintptr_t*>(memory)[0] = a_vtbl; // Unknown vtable location add once REd
			REL::RelocateMember<BSReadWriteLock>(memory, 0x18) = BSReadWriteLock();
		} else {
			REL::RelocateMember<BSReadWriteLock>(memory, 0x10) = BSReadWriteLock();
		}
		return static_cast<ExtraDataList*>(memory);
	}

	struct SpringGrowData {
		Spring amount = Spring(0.0f, 1.0f);
		float addedSoFar = 0.0f;
		bool drain = false;
		ActorHandle actor;

		SpringGrowData(Actor* actor, float amountToAdd, float halfLife) : actor(actor->CreateRefHandle()) {
			amount.value = 0.0f;
			amount.target = amountToAdd;
			amount.halflife = halfLife;
		}
	};

	struct SpringShrinkData {
		Spring amount = Spring(0.0f, 1.0f);
		float addedSoFar = 0.0f;
		ActorHandle actor;

		SpringShrinkData(Actor* actor, float amountToAdd, float halfLife) : actor(actor->CreateRefHandle()) {
			amount.value = 0.0f;
			amount.target = amountToAdd;
			amount.halflife = halfLife;
		}
	};
}

RE::ExtraDataList::~ExtraDataList() {
}

namespace Gts {
	RE::NiPoint3 RotateAngleAxis(const RE::NiPoint3& vec, const float angle, const RE::NiPoint3& axis) {
		float S = sin(angle);
		float C = cos(angle);

		const float XX = axis.x * axis.x;
		const float YY = axis.y * axis.y;
		const float ZZ = axis.z * axis.z;

		const float XY = axis.x * axis.y;
		const float YZ = axis.y * axis.z;
		const float ZX = axis.z * axis.x;

		const float XS = axis.x * S;
		const float YS = axis.y * S;
		const float ZS = axis.z * S;

		const float OMC = 1.f - C;

		return RE::NiPoint3(
			(OMC * XX + C) * vec.x + (OMC * XY - ZS) * vec.y + (OMC * ZX + YS) * vec.z,
			(OMC * XY + ZS) * vec.x + (OMC * YY + C) * vec.y + (OMC * YZ - XS) * vec.z,
			(OMC * ZX - YS) * vec.x + (OMC * YZ + XS) * vec.y + (OMC * ZZ + C) * vec.z
			);
	}

	Actor* GetActorPtr(Actor* actor) {
		return actor;
	}

	Actor* GetActorPtr(Actor& actor) {
		return &actor;
	}

	Actor* GetActorPtr(ActorHandle& actor) {
		if (!actor) {
			return nullptr;
		}
		return actor.get().get();
	}
	Actor* GetActorPtr(const ActorHandle& actor) {
		if (!actor) {
			return nullptr;
		}
		return actor.get().get();
	}
	Actor* GetActorPtr(FormID formId) {
		Actor* actor = TESForm::LookupByID<Actor>(formId);
		if (!actor) {
			return nullptr;
		}
		return actor;
	}

	Actor* GetCharContActor(bhkCharacterController* charCont) {
		for (auto actor: find_actors()) {
			if (charCont == actor->GetCharController()) {
				return actor;
			}
		}
		// Sadly feels like that's the only reliable way to get the actor
		// Possible other method (complex, Meh321 from RE Discord):
		// - hook ctor and dtor of the bhkCharacterController, there's more info in the constructor 
		//  (like which object root node it's created for) and store custom formid -> ptr lookup (or other way around) 
		//  from which you can get it later
		//  bhkCharProxyControllerCinfo+0x10 is root NiNode* of TESObjectREFR
		return nullptr;
	}

	void Task_AdjustHalfLifeTask(Actor* tiny, float halflife, double revert_after) {
		auto& Persist = Persistent::GetSingleton();
		auto actor_data = Persist.GetData(tiny);
		float old_halflife = 0.0f;
		if (actor_data) {
			old_halflife = actor_data->half_life; // record old half life
			actor_data->half_life = halflife;
		}

		double Start = Time::WorldTimeElapsed();
		ActorHandle tinyhandle = tiny->CreateRefHandle();
		std::string name = std::format("AdjustHalfLife_{}", tiny->formID);
		TaskManager::Run(name, [=](auto& progressData) {
			if (!tinyhandle) {
				return false;
			}
			auto tinyref = tinyhandle.get().get();
			double timepassed = Time::WorldTimeElapsed() - Start;
			if (timepassed > revert_after) {
				if (actor_data) {
					actor_data->half_life = old_halflife;
				}
				return false;
			}

			return true;
		});
	}

	float GetLaunchPower(Actor* giant, float sizeRatio) {
		// https://www.desmos.com/calculator/wh0vwgljfl
		SoftPotential launch {
			.k = 1.42f,
			.n = 0.78f,
			.s = 0.6f,
			.a = 0.8f,
		};
		float power = soft_power(sizeRatio, launch);

		if (!giant) {
			return 1.0f;
		}

		float limit = (14.0f * get_visual_scale(giant));

		if (power > limit) {
			return limit;
		}
		
		return power;
	}

	void StartResetTask(Actor* tiny) {
		if (tiny->formID == 0x14) {
			return; //Don't reset Player
		}
		std::string name = std::format("ResetActor_{}", tiny->formID);
		float Start = static_cast<float>(Time::WorldTimeElapsed());
		ActorHandle tinyhandle = tiny->CreateRefHandle();
		TaskManager::Run(name, [=](auto& progressData) {
			if (!tinyhandle) {
				return false;
			}
			auto tiny = tinyhandle.get().get();
			float Finish = static_cast<float>(Time::WorldTimeElapsed());
			float timepassed = Finish - Start;
			if (timepassed < 1.0f) {
				return true; // not enough time has passed yet
			}
			EventDispatcher::DoResetActor(tiny);
			return false; // stop task, we reset the actor
		});
	}

	void PlayMoanSound(Actor* actor, float volume) {
		if (IsFemale(actor) && IsHuman(actor)) {
			float falloff = 0.125f * get_visual_scale(actor);
			Runtime::PlaySoundAtNode_FallOff("MoanSound", actor, volume, 1.0f, "NPC Head [Head]", falloff);
		}
	}

	void PlayLaughSound(Actor* actor, float volume, int type) {
		float falloff = 0.125f * get_visual_scale(actor);
		if (IsFemale(actor) && IsHuman(actor)) {
			if (type == 2) {
				Runtime::PlaySoundAtNode_FallOff("LaughSound_Type2", actor, volume, 1.0f, "NPC Head [Head]", falloff);
			} else {
				Runtime::PlaySoundAtNode_FallOff("LaughSound_Type1", actor, volume, 1.0f, "NPC Head [Head]", falloff);
			}
		}
	}


	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//                                 G T S   ST A T E S  B O O L S                                                                      //
	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	bool BehaviorGraph_DisableHH(Actor* actor) { // should .dll disable HH if Behavior Graph has HH Disable data?
		bool disable = false;
		actor->GetGraphVariableBool("GTS_DisableHH", disable);
		if (actor->formID == 0x14 && IsFirstPerson()) {
			return false;
		}
		bool anims = AnimationsInstalled(actor);
		if (!anims) {
			return false; // prevent hh from being disabled if there's no Nemesis Generation
		}

		return disable;
	}

	bool IsEquipBusy(Actor* actor) {
		auto profiler = Profilers::Profile("ActorUtils: IsEquipBusy");
		int State;
		actor->GetGraphVariableInt("currentDefaultState", State);
		if (State >= 10 && State <= 20) {
			return true;
		}
		return false;
	}

	bool IsRagdolled(Actor* actor) {
		bool ragdoll = actor->IsInRagdollState();
		return ragdoll;
	}
	bool IsGrowing(Actor* actor) {
		bool Growing = false;
		actor->GetGraphVariableBool("GTS_IsGrowing", Growing);
		return Growing;
	}
	
	bool IsChangingSize(Actor* actor) { // Used to disallow growth/shrink during specific animations
		bool Growing = false;
		bool Shrinking = false;
		actor->GetGraphVariableBool("GTS_IsGrowing", Growing);
		actor->GetGraphVariableBool("GTS_IsShrinking", Shrinking);

		return Growing || Shrinking;
	}

	bool IsProning(Actor* actor) {
		bool prone = false;
		if (actor) {
			auto transient = Transient::GetSingleton().GetData(actor);
			actor->GetGraphVariableBool("GTS_IsProne", prone);
			if (actor->formID == 0x14 && actor->IsSneaking() && IsFirstPerson() && transient) {
				return transient->FPProning; // Because we have no FP behaviors, 
				// ^ it is Needed to fix proning being applied to FP even when Prone is off
			}
		}
		return prone;
	}

	bool IsCrawling(Actor* actor) {
		bool crawl = false;
		if (actor) {
			auto transient = Transient::GetSingleton().GetData(actor);
			actor->GetGraphVariableBool("GTS_IsCrawling", crawl);
			if (actor->formID == 0x14 && actor->IsSneaking() && IsFirstPerson() && transient) {
				return transient->FPCrawling; // Needed to fix crawling being applied to FP even when Prone is off
			}
			return actor->IsSneaking() && crawl;
		}
		return false;
	}

	bool IsInBalanceMode() {
		return SizeManager::GetSingleton().BalancedMode() > 1.0f;
	}

	bool IsHugCrushing(Actor* actor) {
		bool IsHugCrushing = false;
		actor->GetGraphVariableBool("IsHugCrushing", IsHugCrushing);
		return IsHugCrushing;
	}

	bool IsHugHealing(Actor* actor) {
		bool IsHugHealing = false;
		actor->GetGraphVariableBool("GTS_IsHugHealing", IsHugHealing);
		return IsHugHealing;
	}

	bool IsVoring(Actor* giant) {
		bool Voring;
		giant->GetGraphVariableBool("GTS_IsVoring", Voring);
		return Voring;
	}

	bool IsHuggingFriendly(Actor* actor) {
		bool friendly = false;
		actor->GetGraphVariableBool("GTS_IsFollower", friendly);
		return friendly;
	}

	bool IsTransitioning(Actor* actor) { // reports sneak transition to crawl
		bool transition = false;
		actor->GetGraphVariableBool("GTS_Transitioning", transition);
		return transition;
	}

	bool IsFootGrinding(Actor* actor) {
		bool grind = false;
		actor->GetGraphVariableBool("GTS_IsFootGrinding", grind);
		return grind;
	}

	bool IsJumping(Actor* actor) {
		bool jumping = false;
		actor->GetGraphVariableBool("bInJumpState", jumping);
		return jumping;
	}

	bool IsBeingHeld(Actor* giant, Actor* tiny) {
		auto grabbed = Grab::GetHeldActor(giant);
		
		if (grabbed) {
			if (grabbed == tiny) {
				return true;
			}
		}
		
		auto transient = Transient::GetSingleton().GetData(tiny);
		if (transient) {
			return transient->being_held && !tiny->IsDead();
		}
		return false;
	}

	bool IsBetweenBreasts(Actor* actor) {
		auto transient = Transient::GetSingleton().GetData(actor);
		if (transient) {
			return transient->is_between_breasts;
		}
		return false;
	}

	bool IsTransferingTiny(Actor* actor) { // Reports 'Do we have someone grabed?'
		int grabbed = 0;
		actor->GetGraphVariableInt("GTS_GrabbedTiny", grabbed);
		return grabbed > 0;
	}

	bool IsUsingThighAnimations(Actor* actor) { // Do we currently use Thigh Crush / Thigh Sandwich?
		int sitting = false;
		actor->GetGraphVariableInt("GTS_Sitting", sitting);
		return sitting > 0;
	}

	bool IsSynced(Actor* actor) {
		bool sync = false;
		actor->GetGraphVariableBool("bIsSynced", sync);
		return sync;
	}

	bool CanDoPaired(Actor* actor) {
		bool paired = false;
		actor->GetGraphVariableBool("GTS_CanDoPaired", paired);
		return paired;
	}


	bool IsThighCrushing(Actor* actor) { // Are we currently doing Thigh Crush?
		int crushing = 0;
		actor->GetGraphVariableInt("GTS_IsThighCrushing", crushing);
		return crushing > 0;
	}

	bool IsThighSandwiching(Actor* actor) { // Are we currently Thigh Sandwiching?
		int sandwiching = 0;
		actor->GetGraphVariableInt("GTS_IsThighSandwiching", sandwiching);
		return sandwiching > 0;
	}

	bool IsBeingEaten(Actor* tiny) {
		auto transient = Transient::GetSingleton().GetData(tiny);
		if (transient) {
			return transient->about_to_be_eaten;
		}
		return false;
	}

	bool IsGtsBusy(Actor* actor) {
		auto profiler = Profilers::Profile("ActorUtils: IsGtsBusy"); 
		bool GTSBusy = false;
		actor->GetGraphVariableBool("GTS_Busy", GTSBusy);

		bool Busy = GTSBusy && !CanDoCombo(actor);
		return Busy;
	}

	bool IsStomping(Actor* actor) {
		bool Stomping = false;
		actor->GetGraphVariableBool("GTS_IsStomping", Stomping);

		return Stomping;
	}

	bool IsInCleavageState(Actor* actor) { // For GTS 
		bool Cleavage = false;

		actor->GetGraphVariableBool("GTS_IsBoobing", Cleavage);

		return Cleavage;
	}

	bool IsCleavageZIgnored(Actor* actor) {
		bool ignored = false;

		actor->GetGraphVariableBool("GTS_OverrideZ", ignored);

		return ignored;
	}

	bool IsInsideCleavage(Actor* actor) { // For tinies
		bool InCleavage = false;

		actor->GetGraphVariableBool("GTS_IsinBoobs", InCleavage);

		return InCleavage;
	}
	
	bool IsKicking(Actor* actor) {
		bool Kicking = false;
		actor->GetGraphVariableBool("GTS_IsKicking", Kicking);

		return Kicking;
	}

	bool IsTrampling(Actor* actor) {
		bool Trampling = false;
		actor->GetGraphVariableBool("GTS_IsTrampling", Trampling);

		return Trampling;
	}

	bool CanDoCombo(Actor* actor) {
		bool Combo;
		actor->GetGraphVariableBool("GTS_CanCombo", Combo);
		return Combo;
	}

	bool IsCameraEnabled(Actor* actor) {
		bool Camera;
		actor->GetGraphVariableBool("GTS_VoreCamera", Camera);
		return Camera;
	}

	bool IsCrawlVoring(Actor* actor) {
		bool Voring;
		actor->GetGraphVariableBool("GTS_IsCrawlVoring", Voring);
		return Voring;//Voring;
	}

	bool IsButtCrushing(Actor* actor) {
		bool ButtCrushing;
		actor->GetGraphVariableBool("GTS_IsButtCrushing", ButtCrushing);
		return ButtCrushing;
	}

	bool ButtCrush_IsAbleToGrow(Actor* actor, float limit) {
		auto transient = Transient::GetSingleton().GetData(actor);
		float stamina = GetAV(actor, ActorValue::kStamina);
		if (stamina <= 4.0f) {
			return false;
		}
		if (transient) {
			return transient->ButtCrushGrowthAmount < limit;
		}
		return false;
	}

	bool IsBeingGrinded(Actor* actor) {
		auto transient = Transient::GetSingleton().GetData(actor);
		bool grinded = false;
		actor->GetGraphVariableBool("GTS_BeingGrinded", grinded);
		if (transient) {
			return transient->being_foot_grinded;
		}
		return grinded;
	}

	bool IsHugging(Actor* actor) {
		bool hugging = false;
		actor->GetGraphVariableBool("GTS_Hugging", hugging);
		return hugging;
	}

	bool IsBeingHugged(Actor* actor) {
		bool hugged = false;
		actor->GetGraphVariableBool("GTS_BeingHugged", hugged);
		return hugged;
	}

	bool CanDoButtCrush(Actor* actor, bool apply_cooldown) {
		bool Allow = IsActionOnCooldown(actor, CooldownSource::Action_ButtCrush);

		if (!Allow && apply_cooldown) { // send it to cooldown if it returns 'not busy'
			ApplyActionCooldown(actor, CooldownSource::Action_ButtCrush);
		}

		return !Allow; // return flipped OnCooldown. By default it false, we flip it so it returns True (Can perform butt crush)
	}

	bool GetCameraOverride(Actor* actor) {
		if (actor->formID == 0x14) {
			auto transient = Transient::GetSingleton().GetData(actor);
			if (transient) {
				return transient->OverrideCamera;
			}
			return false;
		}
		return false;
	}



	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//                                 G T S   ST A T E S  O T H E R                                                                      //
	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


	bool IsGrowthSpurtActive(Actor* actor) {
		if (!Runtime::HasPerkTeam(actor, "GrowthOfStrength")) {
			return false;
		}
		if (HasGrowthSpurt(actor)) {
			return true;
		}
		return false;
	}

	bool HasGrowthSpurt(Actor* actor) {
		bool Growth1 = Runtime::HasMagicEffect(actor, "GrowthSpurt1");
		bool Growth2 = Runtime::HasMagicEffect(actor, "GrowthSpurt2");
		bool Growth3 = Runtime::HasMagicEffect(actor, "GrowthSpurt3");
		if (Growth1 || Growth2 || Growth3) {
			return true;
		} else {
			return false;
		}
	}

	bool InBleedout(Actor* actor) {
		return actor->AsActorState()->IsBleedingOut();
	}

	bool AllowStagger(Actor* giant, Actor* tiny) {
		if (Persistent::GetSingleton().allow_stagger == true) {
			return true; // Allow it
		} else if (Persistent::GetSingleton().allow_stagger == false) {
			bool ProtectGTS = giant->formID == 0x14 || IsTeammate(giant);
			bool ProtectTiny = tiny->formID == 0x14 || IsTeammate(tiny);
			//log::info("GTS {}: {}", giant->GetDisplayFullName(), ProtectGTS);
			//log::info("Tiny {}: {}", tiny->GetDisplayFullName(), ProtectTiny);
			if (ProtectGTS && ProtectTiny) {
				return false; // Protect
			}
			return true;
		}
		return true;
	}

	bool IsMechanical(Actor* actor) {
		bool dwemer = Runtime::HasKeyword(actor, "DwemerKeyword");
		return dwemer;
	}

	bool IsHuman(Actor* actor) { // Check if Actor is humanoid or not. Currently used for Hugs Animation and for playing moans
		bool vampire = Runtime::HasKeyword(actor, "VampireKeyword");
		bool dragon = Runtime::HasKeyword(actor, "DragonKeyword");
		bool animal = Runtime::HasKeyword(actor, "AnimalKeyword");
		bool dwemer = Runtime::HasKeyword(actor, "DwemerKeyword");
		bool undead = Runtime::HasKeyword(actor, "UndeadKeyword");
		bool creature = Runtime::HasKeyword(actor, "CreatureKeyword");
		if (!dragon && !animal && !dwemer && !undead && !creature) {
			return true; // Detect non-vampire
		} if (!dragon && !animal && !dwemer && !creature && undead && vampire) {
			return true; // Detect Vampire
		} else {
			return false;
		}
		return false;
	}

	bool IsBlacklisted(Actor* actor) {
		bool blacklist = Runtime::HasKeyword(actor, "BlackListKeyword");
		return blacklist;
	}

	bool IsGtsTeammate(Actor* actor) {
		return Runtime::HasKeyword(actor, "CountAsFollower");
	}

	void Potion_SetMightBonus(Actor* giant, float value, bool add) {
		auto transient = Transient::GetSingleton().GetData(giant);
		if (transient) {
			if (add) {
				transient->MightValue += value;
			} else {
				transient->MightValue = value;
			}
		}
	}

	float Potion_GetMightBonus(Actor* giant) {
		auto transient = Transient::GetSingleton().GetData(giant);
		if (transient) {
			return transient->MightValue; // return raw bonus
		}
		return 0.0f;
	}

	float Potion_GetSizeMultiplier(Actor* giant) {
		auto transient = Transient::GetSingleton().GetData(giant);
		if (transient) {
			float bonus = std::clamp(transient->potion_max_size, 0.0f, 10.0f);
			return 1.0f + bonus;
		}
		return 1.0f;
	}

	void Potion_SetShrinkResistance(Actor* giant, float value) {
		auto transient = Transient::GetSingleton().GetData(giant);
		if (transient) {
			transient->ShrinkResistance = value;
		}
	}

	float Potion_GetShrinkResistance(Actor* giant) {
		auto transient = Transient::GetSingleton().GetData(giant);
		float Resistance = 1.0f;
		if (transient) {
			Resistance -= transient->ShrinkResistance;
		}
		if (Resistance <= 0.25f) {
			Resistance = 0.25f; // cap it just in case
		}
		return Resistance;
	}

	void Potion_SetUnderGrowth(Actor* actor, bool set) {
		auto transient = Transient::GetSingleton().GetData(actor);
		if (transient) {
			transient->GrowthPotion = set;
		}
	}

	bool Potion_IsUnderGrowthPotion(Actor* actor) {
		bool UnderGrowth = false;
		auto transient = Transient::GetSingleton().GetData(actor);
		if (transient) {
			UnderGrowth = transient->GrowthPotion;
		}
		return UnderGrowth;
	}

	bool IsInsect(Actor* actor, bool performcheck) {
		bool Check = Persistent::GetSingleton().AllowInsectVore;
		if (performcheck && Check) {
			return false;
		}
		bool Spider = Runtime::IsRace(actor, "FrostbiteSpiderRace");
		bool SpiderGiant = Runtime::IsRace(actor, "FrostbiteSpiderRaceGiant");
		bool SpiderLarge = Runtime::IsRace(actor, "FrostbiteSpiderRaceLarge");
		bool ChaurusReaper = Runtime::IsRace(actor, "ChaurusReaperRace");
		bool Chaurus = Runtime::IsRace(actor, "ChaurusRace");
		bool ChaurusHunterDLC = Runtime::IsRace(actor, "DLC1ChaurusHunterRace");
		bool ChaurusDLC = Runtime::IsRace(actor, "DLC1_BF_ChaurusRace");
		bool ExplSpider = Runtime::IsRace(actor, "DLC2ExpSpiderBaseRace");
		bool ExplSpiderPackMule = Runtime::IsRace(actor, "DLC2ExpSpiderPackmuleRace");
		bool AshHopper = Runtime::IsRace(actor, "DLC2AshHopperRace");
		if (Spider||SpiderGiant||SpiderLarge||ChaurusReaper||Chaurus||ChaurusHunterDLC||ChaurusDLC||ExplSpider||ExplSpiderPackMule||AshHopper) {
			return true;
		} else {
			return false;
		}
		return false;
	}

	bool IsFemale(Actor* actor) {
		auto base = actor->GetActorBase();
		int sex = 0;
		if (base) {
			if (base->GetSex()) {
				sex = base->GetSex();
				//log::info("{} Is Female: {}", actor->GetDisplayFullName(), static_cast<bool>(sex));
			}
		}
		return sex; // Else return false
	}

	bool IsDragon(Actor* actor) {
		if (Runtime::HasKeyword(actor, "DragonKeyword")) {
			return true;
		}
		if (Runtime::IsRace(actor, "dragonRace")) {
			return true;
		} 
		return false;
	}

	bool IsGiant(Actor* actor) {
		return Runtime::IsRace(actor, "GiantRace");
	}

	bool IsMammoth(Actor* actor) {
		return Runtime::IsRace(actor, "MammothRace");
	}

	bool IsLiving(Actor* actor) {
		bool IsDraugr = Runtime::HasKeyword(actor, "UndeadKeyword");
		bool IsDwemer = Runtime::HasKeyword(actor, "DwemerKeyword");
		bool IsVampire = Runtime::HasKeyword(actor, "VampireKeyword");
		if (IsVampire) {
			return true;
		}
		if (IsDraugr || IsDwemer) {
			return false;
		} else {
			return true;
		}
		return true;
	}

	bool IsUndead(Actor* actor, bool PerformCheck) {
		bool IsDraugr = Runtime::HasKeyword(actor, "UndeadKeyword");
		bool Check = Persistent::GetSingleton().AllowUndeadVore;
		if (Check && PerformCheck) {
			return false;
		}
		return IsDraugr;
	}

	bool WasReanimated(Actor* actor) { // must be called while actor is still alive, else it will return false.
		bool reanimated = false;
		auto transient = Transient::GetSingleton().GetData(actor);
		if (transient) {
			reanimated = transient->WasReanimated;
		}
		return reanimated;
	}

	bool IsFlying(Actor* actor) {
		bool flying = false;
		if (actor) {
			flying = actor->AsActorState()->IsFlying();
		}
		return flying;
	}

	bool IsHostile(Actor* giant, Actor* tiny) {
		return tiny->IsHostileToActor(giant);
	}

	bool CanPerformAnimationOn(Actor* giant, Actor* tiny, bool HugCheck) {
		
		bool Busy = IsBeingGrinded(tiny) || IsBeingHugged(tiny) || IsHugging(giant);
		// If any of these is true = we disallow animation

		bool Teammate = IsTeammate(tiny);
		bool essential = IsEssential(giant, tiny); // Teammate check is done here
		bool hostile = IsHostile(giant, tiny);
		bool no_protection = Persistent::GetSingleton().FollowerInteractions;
		bool Ignore_Protection = (HugCheck && giant->formID == 0x14 && Runtime::HasPerk(giant, "HugCrush_LovingEmbrace"));
		bool allow_teammate = (giant->formID != 0x14 && no_protection && IsTeammate(tiny) && IsTeammate(giant));

		if (IsFlying(tiny)) {
			return false; // Disallow to do stuff with flying dragons
		}

		if (Busy) {
			return false;
		}
		if (Ignore_Protection) {
			return true;
		}
		if (allow_teammate) { // allow if type is (teammate - teammate), and if bool is true
			return true;
		} else if (essential) { // disallow to perform on essentials
			return false;
		} else if (hostile) { // always allow for non-essential enemies. Will return true if Teammate is hostile towards someone (even player)
			return true;
		} else if (!Teammate) { // always allow for non-teammates
			return true;
		} else {
			return true; // else allow
		}
	}

	bool IsEssential(Actor* giant, Actor* actor) {
		bool essential = actor->IsEssential() && Runtime::GetBool("ProtectEssentials");
		bool protectfollowers = Persistent::GetSingleton().FollowerProtection;
		bool teammate = IsTeammate(actor);
		if (actor->formID == 0x14) {
			return false; // we don't want to make the player immune
		} if (!teammate && essential) {
			return true;
		} else if (teammate && protectfollowers) {
			if (IsHostile(giant, actor)) {
				return false;
			} else if (IsHostile(actor, giant)) {
				return false;
			} else {
				return true;
			}
		} else {
			return false;
		}
	}

	bool IsHeadtracking(Actor* giant) { // Used to report True when we lock onto something, should be Player Exclusive.
		//Currently used to fix TDM mesh issues when we lock on someone.
		bool tracking = false;
		bool headtracking = false;
		if (giant->formID == 0x14) {
			giant->GetGraphVariableBool("TDM_TargetLock", tracking); // get HT value, requires newest versions of TDM to work properly
		} //else {
			//giant->GetGraphVariableBool("bHeadtracking", headtracking);
		//}
		return tracking;
	}

	bool AnimationsInstalled(Actor* giant) {
		bool installed = false;
		giant->GetGraphVariableBool("GTS_Installed", installed);
		return installed;
	}

	bool IsInGodMode(Actor* giant) {
		if (giant->formID != 0x14) {
			return false;
		}
		REL::Relocation<bool*> singleton{ RELOCATION_ID(517711, 404238) };
		return *singleton;
	}

	bool IsFreeCameraEnabled() {
		bool tfc = false;
		auto camera = PlayerCamera::GetSingleton();
		if (camera) {
			if (camera->IsInFreeCameraMode()) {
				tfc = true;
			}
		}
		return tfc;
	}

	bool SizeRaycastEnabled() {
		return Persistent::GetSingleton().SizeRaycast_Enabled;
	}

	bool IsDebugEnabled() {
		return Runtime::GetBool("EnableDebugOverlay"); // used for debug mode of collisions and such
	}

	bool CanDoDamage(Actor* giant, Actor* tiny, bool HoldCheck) {
		if (HoldCheck) {
			if (IsBeingHeld(giant, tiny)) {
				return false;
			}
		}

		bool hostile = (IsHostile(giant, tiny) || IsHostile(tiny, giant));
		bool NPC = Persistent::GetSingleton().NPCEffectImmunity;
		bool PC = Persistent::GetSingleton().PCEffectImmunity;

		if (NPC && giant->formID == 0x14 && (IsTeammate(tiny)) && !hostile) {
			return false; // Protect NPC's against player size-related effects
		}
		if (NPC && (IsTeammate(giant)) && (IsTeammate(tiny))) {
			return false; // Disallow NPC's to damage each-other if they're following Player
		}
		if (PC && tiny->formID == 0x14 && (IsTeammate(giant)) && !hostile) {
			return false; // Protect Player against friendly NPC's damage
		}
		return true;
	}

	void Attachment_SetTargetNode(Actor* giant, AttachToNode Node) {
		auto transient = Transient::GetSingleton().GetData(giant);
		if (transient) {
			transient->AttachmentNode = Node;
		}
	}

	AttachToNode Attachment_GetTargetNode(Actor* giant) {
		auto transient = Transient::GetSingleton().GetData(giant);
		if (transient) {
			return transient->AttachmentNode;
		}
		return AttachToNode::None;
	}

	void ControlAnother(Actor* target, bool reset) {
		Actor* player = PlayerCharacter::GetSingleton();
		auto transient = Transient::GetSingleton().GetData(player);
		if (transient) {
			if (reset) {
				transient->IsInControl = nullptr;
				return;
			} else {
				transient->IsInControl = target;
			}
		}
	}

	void RecordSneaking(Actor* actor) {
		auto transient = Transient::GetSingleton().GetData(actor);
		bool sneaking = actor->IsSneaking();
		if (transient) {
			transient->was_sneaking = sneaking;
		}
	}

	void SetSneaking(Actor* actor, bool override_sneak, int enable) {
		if (override_sneak) {
			actor->AsActorState()->actorState1.sneaking = enable;
		} else {
			auto transient = Transient::GetSingleton().GetData(actor);
			if (transient) {
				actor->AsActorState()->actorState1.sneaking = transient->was_sneaking;
			}
		}
	}

	void SetWalking(Actor* actor, int enable) {
		actor->AsActorState()->actorState1.walking = enable;
		actor->AsActorState()->actorState1.running = 0;
		actor->AsActorState()->actorState1.sprinting = 0;
	}

	Actor* GetPlayerOrControlled() {
		Actor* controlled = PlayerCharacter::GetSingleton();
		auto transient = Transient::GetSingleton().GetData(controlled);
		if (transient) {
			if (transient->IsInControl != nullptr) {
				return transient->IsInControl;
			}
		}
		return controlled;
	}



	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//                                 G T S   A C T O R   F U N C T I O N S                                                              //
	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	float GetDamageSetting() {
		return Persistent::GetSingleton().size_related_damage_mult;
	}

	float GetFallModifier(Actor* giant) {
		auto transient = Transient::GetSingleton().GetData(giant);
		float fallmod = 1.0f;
		if (transient) {
			fallmod = transient->FallTimer;
			//log::info("Fall mult :{}", transient->FallTimer);
		}
		return fallmod;
	}

	std::vector<Actor*> Vore_GetMaxVoreCount(Actor* giant, std::vector<Actor*> actors) {
		float capacity = 1.0f;
		std::vector<Actor*> vories = {};
		if (Runtime::HasPerkTeam(giant, "EnhancedCapacity")) {
			capacity = 3.0f * get_visual_scale(giant);
			if (HasSMT(giant)) {
				capacity *= 3.0f;
			}
		} 
		for (auto target: actors) {
			if (capacity <= 1.0f) {
				capacity = 1.0f;
				vories.push_back(target);
				//log::info("(Return) Max Vore for {} is {}", giant->GetDisplayFullName(), vories.size());
				return vories;
			}
			float decrease = get_target_scale(target) * 12.0f;
			capacity -= decrease;
			vories.push_back(target);
		}  
		//log::info("Max Vore for {} is {}", giant->GetDisplayFullName(), vories.size());
		return vories;
	}

	float Ench_Aspect_GetPower(Actor* giant) { 
		float aspect = SizeManager::GetSingleton().GetEnchantmentBonus(giant) * 0.01f;
		return aspect;
	}
	float Ench_Hunger_GetPower(Actor* giant) {
		float hunger = SizeManager::GetSingleton().GetSizeHungerBonus(giant) * 0.01f;
		return hunger;
	}

	float Perk_GetSprintShrinkReduction(Actor* actor) {
		float resistance = 1.0f;
		if (actor->AsActorState()->IsSprinting() && Runtime::HasPerkTeam(actor, "QuickApproach")) {
			resistance -= 0.20f;
		}
		return resistance;
	}

	float GetDamageResistance(Actor* actor) {
		return AttributeManager::GetSingleton().GetAttributeBonus(actor, ActorValue::kHealth);
	}

	float GetDamageMultiplier(Actor* actor) {
		return AttributeManager::GetSingleton().GetAttributeBonus(actor, ActorValue::kAttackDamageMult);
	}

	float Damage_CalculateSizeDamage(Actor* giant, Actor* tiny) {
		return GetDamageMultiplier(giant) * GetDamageResistance(tiny);
	}

	float GetSizeDifference(Actor* giant, Actor* tiny, SizeType Type, bool Check_SMT, bool HH) {
		float hh_gts = 0.0f; 
		float hh_tiny = 0.0f;

		float GiantScale = 1.0f;
		float TinyScale = 1.0f;

		if (HH) { // Apply HH only in cases when we need it, such as damage and hugs
			hh_gts = HighHeelManager::GetHHOffset(giant)[2] * 0.01f;
			hh_tiny = HighHeelManager::GetHHOffset(tiny)[2] * 0.01f;
		}
		
		switch (Type) {
			case SizeType::GiantessScale: 
				GiantScale = get_giantess_scale(giant) + hh_gts;
				TinyScale = get_giantess_scale(tiny) + hh_tiny;
			break;
			case SizeType::VisualScale: 
				GiantScale = (get_visual_scale(giant) + hh_gts) * GetSizeFromBoundingBox(giant);
				TinyScale = (get_visual_scale(tiny) + hh_tiny) * GetSizeFromBoundingBox(tiny);
			break;
			case SizeType::TargetScale:
				GiantScale = get_target_scale(giant) + hh_gts;
				TinyScale = get_target_scale(tiny) + hh_tiny;
			break;
		}

		if (Check_SMT) {
			if (HasSMT(giant)) {
				GiantScale += 10.2f;
			} 
		}

		if (tiny->formID == 0x14 && HasSMT(tiny)) {
			TinyScale += 1.50f;
		}

		float Difference = GiantScale/TinyScale;
		/*if (giant->formID == 0x14 && !tiny->IsDead()) {
			log::info("Size Difference between {} and {} is {}", giant->GetDisplayFullName(), tiny->GetDisplayFullName(), Difference);
			log::info("Tiny Data: TS: {} ; HH: {} ; BB: {}, target: {}", TinyScale, hh_tiny, GetSizeFromBoundingBox(tiny), get_target_scale(tiny));
			log::info("GTS Data: TS {} ; HH: {} BB :{}", GiantScale, hh_gts, GetSizeFromBoundingBox(giant));
		}*/

		return Difference;
	}

	float GetActorWeight(Actor* giant, bool metric) {
		float hh = HighHeelManager::GetBaseHHOffset(giant)[2]/100;
		float scale = get_visual_scale(giant);
		float smt = 1.0f;
		float totalscale = scale + (hh*0.10f * scale);
		float actorweight = 1.0f + giant->GetWeight()/300;
		float weight;
		if (metric) { // 70.1f kg as a base
			weight = 70.1f * actorweight * (totalscale * totalscale * totalscale);
		} else {
			weight = (70.1f * actorweight * (totalscale * totalscale * totalscale)) * 2.205f;
		} if (HasSMT(giant)) {
			smt = 6.0f;
		}
		return weight * smt;
	}

	float GetActorHeight(Actor* giant, bool metric) {
		float hh = HighHeelManager::GetBaseHHOffset(giant)[2]/100;
		float bb = GetSizeFromBoundingBox(giant);
		float scale = get_visual_scale(giant);
		float smt = 1.0f;
		float height;
		if (metric) { // 1.82f m as a base
			height = 1.82f * bb * scale + (hh * scale); // meters
		} else {
			height = (1.82f * bb * scale + (hh * scale)) * 3.28f; // ft
		}
		return height;
	}

	float GetSizeFromBoundingBox(Actor* tiny) {
		auto profiler = Profilers::Profile("ActorUtils: GetSizeFromBoundingBox");
		float sc = get_bounding_box_to_mult(tiny);
		return sc;
	}

	float GetRoomStateScale(Actor* giant) {
		// Goal is to make us effectively smaller during these checks, so RayCast won't adjust our height unless we're truly too big
		float Normal = 1.0f;
		float Reduction = 1.0f;

		if (IsProning(giant)) {
			return 0.30f;
		} else if (IsCrawling(giant)) {
			return 0.46f;
		} else if (giant->IsSneaking()) {
			Reduction = 0.70f;
		} else {
			Reduction = 1.0f;
		}
		float HH = (HighHeelManager::GetBaseHHOffset(giant).Length()/100)/1.82f; // Get HH value and convert it to meters
		return (Normal + HH) * Reduction;
	}

	float GetProneAdjustment() {
		auto player = PlayerCharacter::GetSingleton();
		float value = 1.0f;
		if (IsProning(player)) {
			return 0.18f;
		} else if (IsCrawling(player)) {
			value = std::clamp(Runtime::GetFloat("ProneOffsetFP"), 0.10f, 1.0f);
		}
		
		return value;
	}

	void SpawnActionIcon(Actor* giant) {
		if (!giant) {
			return;
		}
		bool enabled = Persistent::GetSingleton().EnableIcons;
		if (!enabled) {
			return;
		}
		static Timer EffectTimer = Timer(3.0f);
		if (giant->formID == 0x14 && EffectTimer.ShouldRunFrame()) {
			NiPoint3 NodePosition = giant->GetPosition();

			float giantScale = get_visual_scale(giant);
			auto huggedActor = HugShrink::GetHuggiesActor(giant);

			const float BASE_DISTANCE = 124.0f;
			float CheckDistance = BASE_DISTANCE * giantScale;

			if (IsCrawling(giant)) {
				CheckDistance *= 1.5f;
			}

			if (IsDebugEnabled()) {
				DebugAPI::DrawSphere(glm::vec3(NodePosition.x, NodePosition.y, NodePosition.z), CheckDistance, 60, {0.5f, 1.0f, 0.0f, 0.5f});
			}

			for (auto otherActor: find_actors()) {
				if (otherActor != giant) {
					if (otherActor->Is3DLoaded() && !otherActor->IsDead()) {
						float tinyScale = get_visual_scale(otherActor) * GetSizeFromBoundingBox(otherActor);
						float difference = GetSizeDifference(giant, otherActor, SizeType::VisualScale, true, false);
						if (difference > 5.8f || huggedActor) {
							NiPoint3 actorLocation = otherActor->GetPosition();
							if ((actorLocation - NodePosition).Length() < CheckDistance) {
								int nodeCollisions = 0;
								float force = 0.0f;

								auto model = otherActor->GetCurrent3D();

								if (model) {
									VisitNodes(model, [&nodeCollisions, &force, NodePosition, CheckDistance](NiAVObject& a_obj) {
										float distance = (NodePosition - a_obj.world.translate).Length();
										if (distance < CheckDistance) {
											nodeCollisions += 1;
											force = 1.0f - distance / CheckDistance;
											return false;
										}
										return true;
									});
								}
								if (nodeCollisions > 0) {
									auto node = find_node(otherActor, "NPC Root [Root]");
									if (node) {
										auto grabbedActor = Grab::GetHeldActor(giant);
										float correction = 0; 
										if (tinyScale < 1.0f) {
											correction = std::clamp((18.0f / tinyScale) - 18.0f, 0.0f, 144.0f);
										} else {
											correction = (18.0f * tinyScale) - 18.0f;
										}

										float iconScale = std::clamp(tinyScale, 1.0f, 9999.0f) * 2.4f;
										bool Ally = !IsHostile(giant, otherActor) && IsTeammate(otherActor);
										bool HasLovingEmbrace = Runtime::HasPerkTeam(giant, "HugCrush_LovingEmbrace");
										bool Healing = IsHugHealing(giant);

										NiPoint3 Position = node->world.translate;
										float bounding_z = get_bounding_box_z(otherActor);
										if (bounding_z > 0.0f) {
											if (IsCrawling(giant) && IsBeingHugged(otherActor)) {
												bounding_z *= 0.25f; // Move the icon down
											}
											Position.z += (bounding_z * get_visual_scale(otherActor) * 2.35f); // 2.25f to be slightly above the head
											//log::info("For Actor: {}", otherActor->GetDisplayFullName());
											//log::info("---	Position: {}", Vector2Str(Position));
											//log::info("---	Actor Position: {}", Vector2Str(otherActor->GetPosition()));
											//log::info("---	Bounding Z: {}, Bounding Z * Scale: {}", bounding_z, bounding_z * tinyScale);
										} else {
											Position.z -= correction;
										}
										
										if (grabbedActor && grabbedActor == otherActor) {
											//do nothing
										} else if (huggedActor && huggedActor == otherActor && Ally && HasLovingEmbrace && !Healing) {
											SpawnParticle(otherActor, 3.00f, "GTS/UI/Icon_LovingEmbrace.nif", NiMatrix3(), Position, iconScale, 7, node);
										} else if (huggedActor && huggedActor == otherActor && !IsHugCrushing(giant) && !Healing) {
											bool LowHealth = (GetHealthPercentage(huggedActor) < GetHugCrushThreshold(giant, otherActor, true));
											bool ForceCrush = Runtime::HasPerkTeam(giant, "HugCrush_MightyCuddles");
											float Stamina = GetStaminaPercentage(giant);
											if (HasSMT(giant) || LowHealth || (ForceCrush && Stamina > 0.75f)) {
												SpawnParticle(otherActor, 3.00f, "GTS/UI/Icon_Hug_Crush.nif", NiMatrix3(), Position, iconScale, 7, node); // Spawn 'can be hug crushed'
											}
										} else if (!IsGtsBusy(giant) && IsEssential(giant, otherActor)) {
											SpawnParticle(otherActor, 3.00f, "GTS/UI/Icon_Essential.nif", NiMatrix3(), Position, iconScale, 7, node); 
											// Spawn Essential icon
										} else if (!IsGtsBusy(giant) && difference >= Action_Crush) {
											if (CanPerformAnimation(giant, 3)) {
												SpawnParticle(otherActor, 3.00f, "GTS/UI/Icon_Crush_All.nif", NiMatrix3(), Position, iconScale, 7, node); 
												// Spawn 'can be crushed and any action can be done'
											} else {
												SpawnParticle(otherActor, 3.00f, "GTS/UI/Icon_Crush.nif", NiMatrix3(), Position, iconScale, 7, node); 
												// just spawn can be crushed, can happen at any quest stage
											}
										} else if (!IsGtsBusy(giant) && difference >= Action_Grab) {
											if (CanPerformAnimation(giant, 3)) {
												SpawnParticle(otherActor, 3.00f, "GTS/UI/Icon_Vore_Grab.nif", NiMatrix3(), Position, iconScale, 7, node); 
												// Spawn 'Can be grabbed/vored'
											} else if (CanPerformAnimation(giant, 2)) {
												SpawnParticle(otherActor, 3.00f, "GTS/UI/Icon_Grab.nif", NiMatrix3(), Position, iconScale, 7, node); 
												// Spawn 'Can be grabbed'
											}
										} else if (!IsGtsBusy(giant) && difference >= Action_Sandwich && CanPerformAnimation(giant, 2)) {
											SpawnParticle(otherActor, 3.00f, "GTS/UI/Icon_Sandwich.nif", NiMatrix3(), Position, iconScale, 7, node); // Spawn 'Can be sandwiched'
										} 
										// 1 = stomps and kicks
										// 2 = Grab and Sandwich
										// 3 = Vore
										// 5 = Others
									}
								}
							}
						}
					}
				}
			}
		}
	}
	void override_actor_scale(Actor* giant, float amt, SizeEffectType type) { // This function overrides gts manager values. 
	    // It ignores half-life, allowing more than 1 growth/shrink sources to stack nicely
		auto Persistent = Persistent::GetSingleton().GetData(giant);
		if (Persistent) {
			float OnTheEdge = 1.0f;
			float scale = get_visual_scale(giant);

			if (amt > 0 && (giant->formID == 0x14 || IsTeammate(giant))) {
				if (scale >= 1.0f) {
					amt /= GetGrowthReduction(scale); // Enabled if BalanceMode is True. Decreases Grow Efficiency.
				}
			} else if (giant->formID == 0x14 && amt - EPS < 0.0f) {
				// If negative change: add stolen attributes
				DistributeStolenAttributes(giant, -amt * GetGrowthReduction(scale)); // Adjust max attributes
			}

			if (giant->formID == 0x14 && type == SizeEffectType::kShrink) {
				OnTheEdge = GetPerkBonus_OnTheEdge(giant, amt); // Player Exclusive
			}

			float target = get_target_scale(giant);
			float max_scale = get_max_scale(giant);
			if (target < max_scale || amt < 0) {
				amt /= game_getactorscale(giant);
				Persistent->target_scale += amt;
				Persistent->visual_scale += amt;
			}
		}
	}

	void update_target_scale(Actor* giant, float amt, SizeEffectType type) { // used to mod scale with perk bonuses taken into account
		float OnTheEdge = 1.0f;
		
		if (amt > 0 && (giant->formID == 0x14 || IsTeammate(giant))) {
			float scale = get_visual_scale(giant);
			if (scale >= 1.0f) {
				amt /= GetGrowthReduction(scale); // Enabled if BalanceMode is True. Decreases Grow Efficiency.
			}
		} else if (giant->formID == 0x14 && amt - EPS < 0.0f) {
			// If negative change: add stolen attributes
			float scale = get_visual_scale(giant);
			DistributeStolenAttributes(giant, -amt * GetGrowthReduction(scale)); // Adjust max attributes
		}

		if (giant->formID == 0x14 && type == SizeEffectType::kShrink) {
			OnTheEdge = GetPerkBonus_OnTheEdge(giant, amt); // Player Exclusive
		}

		mod_target_scale(giant, amt * OnTheEdge); // set target scale value
	}

	float get_update_target_scale(Actor* giant, float amt, SizeEffectType type) { // Used for growth spurt
		float OnTheEdge = 1.0f;
		
		if (amt > 0 && (giant->formID == 0x14 || IsTeammate(giant))) {
			float scale = get_visual_scale(giant);
			if (scale >= 1.0f) {
				amt /= GetGrowthReduction(scale); // Enabled if BalanceMode is True. Decreases Grow Efficiency.
			}
		} else if (giant->formID == 0x14 && amt - EPS < 0.0f) {
			// If negative change: add stolen attributes
			float scale = get_visual_scale(giant);
			DistributeStolenAttributes(giant, -amt * GetGrowthReduction(scale)); // Adjust max attributes
		}
		
		if (giant->formID == 0x14 && type == SizeEffectType::kShrink) {
			OnTheEdge = GetPerkBonus_OnTheEdge(giant, amt); // Player Exclusive
		}

		mod_target_scale(giant, amt * OnTheEdge); // set target scale value

		return amt * OnTheEdge;
	}

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//                                 G T S   S T A T E S  S E T S                                                                       //
	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	void SetBeingHeld(Actor* tiny, bool enable) {
		auto transient = Transient::GetSingleton().GetData(tiny);
		if (transient) {
			transient->being_held = enable;
		}
	}
	void SetProneState(Actor* giant, bool enable) {
		if (giant->formID == 0x14) {
			auto transient = Transient::GetSingleton().GetData(giant);
			if (transient) {
				transient->FPProning = enable;
			}
		}
	}
	void SetBetweenBreasts(Actor* actor, bool enable) {
		auto transient = Transient::GetSingleton().GetData(actor);
		if (transient) {
			transient->is_between_breasts = enable;
		}
	}
	void SetBeingEaten(Actor* tiny, bool enable) {
		auto transient = Transient::GetSingleton().GetData(tiny);
		if (transient) {
			transient->about_to_be_eaten = enable;
		}
	}
	void SetBeingGrinded(Actor* tiny, bool enable) {
		auto transient = Transient::GetSingleton().GetData(tiny);
		if (transient) {
			transient->being_foot_grinded = enable;
		}
	}

	void SetCameraOverride(Actor* actor, bool enable) {
		if (actor->formID == 0x14) {
			auto transient = Transient::GetSingleton().GetData(actor);
			if (transient) {
				transient->OverrideCamera = enable;
			}
		}
	}

	void SetReanimatedState(Actor* actor) {
		auto transient = Transient::GetSingleton().GetData(actor);
		if (!WasReanimated(actor)) { // disallow to override it again if it returned true a frame before
			bool reanimated = actor->AsActorState()->GetLifeState() == ACTOR_LIFE_STATE::kReanimate;
			if (transient) {
				transient->WasReanimated = reanimated;
			}
		}
	}

	bool IsUsingAlternativeStomp(Actor* giant) { // Used for alternative grind
		bool alternative = false;

		giant->GetGraphVariableBool("GTS_IsAlternativeGrind", alternative);

		return alternative;
	}

	void ShutUp(Actor* actor) { // Disallow them to "So anyway i've been fishing today and my dog died" while we do something to them
		if (!actor) {
			return;
		}
		if (actor->formID != 0x14) {
			auto ai = actor->GetActorRuntimeData().currentProcess;
			if (ai) {
				if (ai->high) {
					float Greeting = ai->high->greetingTimer;
					ai->high->greetingTimer = 5;
				}
			}
		}
	}

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	void PlayAnimation(Actor* actor, std::string_view animName) {
		actor->NotifyAnimationGraph(animName);
	}

	void Disintegrate(Actor* actor) {
		std::string taskname = std::format("Disintegrate_{}", actor->formID);
		auto tinyref = actor->CreateRefHandle();
		TaskManager::RunOnce(taskname, [=](auto& update) {
			if (!tinyref) {
				return;
			}
			auto tiny = tinyref.get().get();

			SetCriticalStage(tiny, 4);
		});
	}



	void UnDisintegrate(Actor* actor) {
		//actor->GetActorRuntimeData().criticalStage.reset(ACTOR_CRITICAL_STAGE::kDisintegrateEnd);
	}

	void SetRestrained(Actor* actor) {
		CallFunctionOn(actor, "Actor", "SetRestrained", true);
	}

	void SetUnRestrained(Actor* actor) {
		CallFunctionOn(actor, "Actor", "SetRestrained", false);
	}

	void SetDontMove(Actor* actor) {
		CallFunctionOn(actor, "Actor", "SetDontMove", true);
	}

	void SetMove(Actor* actor) {
		CallFunctionOn(actor, "Actor", "SetDontMove", true);
	}

	void ForceRagdoll(Actor* actor, bool forceOn) {
		if (!actor) {
			return;
		}
		auto charCont = actor->GetCharController();
		if (!charCont) {
			return;
		}
		BSAnimationGraphManagerPtr animGraphManager;
		if (actor->GetAnimationGraphManager(animGraphManager)) {
			for (auto& graph : animGraphManager->graphs) {
				if (graph) {
					if (graph->HasRagdoll()) {
						if (forceOn) {
							graph->AddRagdollToWorld();
							charCont->flags.set(CHARACTER_FLAGS::kFollowRagdoll);
						} else {
							graph->RemoveRagdollFromWorld();
							charCont->flags.reset(CHARACTER_FLAGS::kFollowRagdoll);
						}
					}
				}
			}
		}
	}


	std::vector<hkpRigidBody*> GetActorRB(Actor* actor) {
		std::vector<hkpRigidBody*> results = {};
		auto charCont = actor->GetCharController();
		if (!charCont) {
			return results;
		}

		bhkCharProxyController* charProxyController = skyrim_cast<bhkCharProxyController*>(charCont);
		bhkCharRigidBodyController* charRigidBodyController = skyrim_cast<bhkCharRigidBodyController*>(charCont);
		if (charProxyController) {
			// Player controller is a proxy one
			auto& proxy = charProxyController->proxy;
			hkReferencedObject* refObject = proxy.referencedObject.get();
			if (refObject) {
				hkpCharacterProxy* hkpObject = skyrim_cast<hkpCharacterProxy*>(refObject);

				if (hkpObject) {
					// Not sure what bodies is doing
					for (auto body: hkpObject->bodies) {
						results.push_back(body);
					}
					// // This one appears to be active during combat.
					// // Maybe used for sword swing collision detection
					// for (auto phantom: hkpObject->phantoms) {
					// 	results.push_back(phantom);
					// }
					//
					// // This is the actual shape
					// if (hkpObject->shapePhantom) {
					// 	results.push_back(hkpObject->shapePhantom);
					// }
				}
			}
		} else if (charRigidBodyController) {
			// NPCs seem to use rigid body ones
			auto& characterRigidBody = charRigidBodyController->characterRigidBody;
			hkReferencedObject* refObject = characterRigidBody.referencedObject.get();
			if (refObject) {
				hkpCharacterRigidBody* hkpObject = skyrim_cast<hkpCharacterRigidBody*>(refObject);
				if (hkpObject) {
					if (hkpObject->m_character) {
						results.push_back(hkpObject->m_character);
					}
				}
			}
		}

		return results;
	}

	void PushActorAway(Actor* source, Actor* receiver, float afKnockBackForce) {
		// Force < 1.0f can introduce weird sliding issues to Actors, not recommended to pass force < 1.0f
		if (receiver->IsDead()) {
			return;
		}

		if (afKnockBackForce <= 1.0f) {
			afKnockBackForce = 1.0f;
		} 

		if (source) {
			auto ai = source->GetActorRuntimeData().currentProcess;
			if (ai) {
				if (receiver->Is3DLoaded()) {
					if (source->Is3DLoaded()) {
						NiPoint3 direction = receiver->GetPosition() - source->GetPosition();
						direction = direction / direction.Length();
						typedef void (*DefPushActorAway)(AIProcess *ai, Actor* actor, NiPoint3& direction, float force);
						REL::Relocation<DefPushActorAway> RealPushActorAway{ RELOCATION_ID(38858, 39895) };
						RealPushActorAway(ai, receiver, direction, afKnockBackForce);
					}
				}
			}
		}
	}

	void KnockAreaEffect(TESObjectREFR* source, float afMagnitude, float afRadius) {
		CallFunctionOn(source, "ObjectReference", "KnockAreaEffect", afMagnitude, afRadius);
	}
	
	void ApplyManualHavokImpulse(Actor* target, float afX, float afY, float afZ, float Multiplier) {
		// For this function to work, actor must be pushed away first. 
		// It may be a good idea to wait about 0.05f sec before callind it after actor has been pushed, else it may not work
		hkVector4 impulse = hkVector4(afX * Multiplier, afY * Multiplier, afZ * Multiplier, 1.0f);

		auto collision = target->Get3D(false)->GetCollisionObject();
		if (collision) {
			auto rigidbody = collision->GetRigidBody();
			if (rigidbody) {
				auto body = rigidbody->AsBhkRigidBody();
				if (body) {
					SetLinearImpulse(body, impulse);
					//log::info("Bdy found, Applying impulse {} to {}", Vector2Str(impulse), target->GetDisplayFullName());
				}
			}
		}
	}

	void CompleteDragonQuest(Actor* tiny, ParticleType Type, bool dead) {
		auto pc = PlayerCharacter::GetSingleton();
		auto progressionQuest = Runtime::GetQuest("MainQuest");
		if (progressionQuest) {
			auto stage = progressionQuest->GetCurrentStageID();
			if (stage == 80) {
				auto transient = Transient::GetSingleton().GetData(pc);
				if (transient) {
					Cprint("Quest is Completed");
					transient->dragon_was_eaten = true;
					SpawnCustomParticle(tiny, Type, NiPoint3(), "NPC Root [Root]", 1.0f);
				}
			}
		}
	}
	float GetHighHeelsBonusDamage(Actor* actor, bool multiply) {
		return GetHighHeelsBonusDamage(actor, multiply, 1.0f);
	}
	float GetHighHeelsBonusDamage(Actor* actor, bool multiply, float adjust) {
		auto profiler = Profilers::Profile("ActorUtils: GetHHBonusDamage");
		float value = 0.0f;
		float hh = 0.0f;

		if (actor) {
			if (Runtime::HasPerkTeam(actor, "hhBonus")) {
				hh = HighHeelManager::GetBaseHHOffset(actor).Length()/100;
			}
		} if (multiply) {
			value = 1.0f + (hh * 5.0f * adjust);
		} else {
			value = hh;
		}
		//log::info("For Actor: {}: {}", actor->GetDisplayFullName(), value);
		return value;
	}

	float get_distance_to_actor(Actor* receiver, Actor* target) {
		auto profiler = Profilers::Profile("ActorUtils: GetDistanceToActor");
		if (target) {
			auto point_a = receiver->GetPosition();
			auto point_b = target->GetPosition();
			auto delta = point_a - point_b;
			return delta.Length();
		}
		return std::numeric_limits<float>::max();
		//return 3.4028237E38; // Max float
	}

	void ApplyShake(Actor* caster, float modifier, float radius) {
		if (caster) {
			auto position = caster->GetPosition();
			ApplyShakeAtPoint(caster, modifier, position, 0.0f);
		}
	}

	void ApplyShakeAtNode(Actor* caster, float modifier, std::string_view nodesv) {
		auto node = find_node(caster, nodesv);
		if (node) {
			ApplyShakeAtPoint(caster, modifier, node->world.translate, 0.0f);
		}
	}

	void ApplyShakeAtPoint(Actor* caster, float modifier, const NiPoint3& coords, float duration_override) {
		if (!caster) {
			return;
		}
		// Reciever is always PC, if it is not PC - we do nothing anyways
		Actor* receiver = PlayerCharacter::GetSingleton();
		if (receiver) {
			auto& persist = Persistent::GetSingleton();
			
			float tremor_scale = persist.npc_tremor_scale;
			float might = 1.0f + Potion_GetMightBonus(caster); // Stronger, more impactful shake with Might potion
			
			float distance = (coords - receiver->GetPosition()).Length(); // In that case we apply shake based on actor distance

			float AnimSpeed = AnimationManager::GetAnimSpeed(caster);

			float sourcesize = get_visual_scale(caster);
			float receiversize = get_visual_scale(receiver);

			float sizedifference = sourcesize/receiversize;
			float scale_bonus = 0.0f;

			if (caster->formID == 0x14) {
				distance = get_distance_to_camera(coords); // use player camera distance (for player only)
				tremor_scale = persist.tremor_scale;
				sizedifference = sourcesize;

				if (HasSMT(caster)) {
					sourcesize += 0.2f; // Fix SMT having no shake at x1.0f scale
				}
				if (IsFirstPerson()) {
					tremor_scale *= 0.25f; // Less annoying FP screen shake
				}

				scale_bonus = 0.1f;
			} 

			if (sourcesize < 2.0f) {  // slowly gain power of shakes
				float reduction = (sourcesize - 1.0f);
				if (reduction < 0.0f) {
					reduction = 0.0f;
				}
				modifier *= reduction;
			}

			SoftPotential params {.k = 0.0065f, .n = 3.0f, .s = 1.0f, .o = 0.0f, .a = 0.0f, };
			//https://www.desmos.com/calculator/jeeugm72gn

			sizedifference *= modifier * tremor_scale * might;

			float intensity = soft_core(distance * 2.5f / sizedifference, params);
			float duration = 0.45f * (1 + intensity);

			intensity *= 1 + ((sourcesize * scale_bonus) - scale_bonus);

			if (duration_override > 0) { // Custom extra duration when needed
				duration *= duration_override;
			}

			intensity = std::clamp(intensity, 0.0f, 4.0f);
			duration = std::clamp(duration, 0.0f, 2.6f);

			if (intensity > 0.005f) {
				shake_controller(intensity, intensity, duration);

				auto camera = PlayerCamera::GetSingleton(); // Shake at the camera pos, else it won't always shake properly
				if (camera) {
					shake_camera_at_node(camera->pos, intensity, duration);
				}
			}
		}
	}

	void EnableFreeCamera() {
		auto playerCamera = PlayerCamera::GetSingleton();
		playerCamera->ToggleFreeCameraMode(false);
	}

	bool DisallowSizeDamage(Actor* giant, Actor* tiny) {
		auto transient = Transient::GetSingleton().GetData(giant);
		if (transient) {
			if (transient->Protection == false) {
				return false;
			} 

			bool Hostile = IsHostile(giant, tiny);
			return transient->Protection && !Hostile;
		}
		
		return false;
	}

	bool AllowDevourment() {
		return Persistent::GetSingleton().devourment_compatibility;
	}

	bool AllowCameraTracking() {
		return Persistent::GetSingleton().allow_feetracking;
	}
	bool LessGore() {
		return Persistent::GetSingleton().less_gore;
	}

	bool IsTeammate(Actor* actor) {
		if (!actor) {
			return false;
		}
		if (Runtime::InFaction(actor, "FollowerFaction") || actor->IsPlayerTeammate() || IsGtsTeammate(actor)) {
			return true;
		}
		return false;
	}

	bool EffectsForEveryone(Actor* giant) { // determines if we want to apply size effects for literally every single actor
		if (giant->formID == 0x14) { // don't enable for Player
			return false;
		}
		//float scale = get_visual_scale(giant);
		//if (scale > 1.10f) {
		bool dead = giant->IsDead();
		bool everyone = Runtime::GetBool("PreciseDamageOthers");
		if (!dead && everyone) {
			return true;
		} else {
			return false;
		}
		//}
		return false;
	}

	void ResetCameraTracking() {
		auto player = PlayerCharacter::GetSingleton();
		if (player) {
			auto& sizemanager = SizeManager::GetSingleton();
			sizemanager.SetTrackedBone(player, false, CameraTracking::None);
		}
	}

	void CallDevourment(Actor* giant, Actor* tiny) {
		auto progressionQuest = Runtime::GetQuest("MainQuest");
		if (progressionQuest) {
			CallFunctionOn(progressionQuest, "gtsProgressionQuest", "Devourment", giant, tiny);
		}
	}

	void GainWeight(Actor* giant, float value) {
		if (Persistent::GetSingleton().allow_weight_gain) {
			if (giant->formID == 0x14) {
				std::string_view name = "Vore_Weight";
				auto gianthandle = giant->CreateRefHandle();
				TaskManager::RunOnce(name, [=](auto& progressData) {
					if (!gianthandle) {
						return false;
					}
					auto giantref = gianthandle.get().get();
					float& original_weight = giantref->GetActorBase()->weight;
					if (original_weight >= 100.0f) {
						return false;
					} 
					if (original_weight + value >= 100.0f) {
						original_weight = 100.0f;
					} else {
						original_weight += value;
					}
					giantref->DoReset3D(true);
					return false;
				});
			}
		}
	}
	

	void CallVampire() {
		auto progressionQuest = Runtime::GetQuest("MainQuest");
		if (progressionQuest) {
			CallFunctionOn(progressionQuest, "gtsProgressionQuest", "SatisfyVampire");
		}
	}

	void CallHelpMessage() {
		auto progressionQuest = Runtime::GetQuest("MainQuest");
		if (progressionQuest) {
			CallFunctionOn(progressionQuest, "gtsProgressionQuest", "TrueGiantessMessage");
		}
	}

	void AddCalamityPerk() {
		auto progressionQuest = Runtime::GetQuest("MainQuest");
		if (progressionQuest) {
			CallFunctionOn(progressionQuest, "gtsProgressionQuest", "AddCalamityPerk");
		}
	}

	void AddPerkPoints(float level) {
		auto GtsSkillPerkPoints = Runtime::GetGlobal("GtsSkillPerkPoints");
		if (!GtsSkillPerkPoints) {
			return;
		}
		if (int(level) % 5 == 0) {
			Notify("You've learned a bonus perk point");
			GtsSkillPerkPoints->value += 1.0f;
		}
		if (level == 20 || level == 40) {
			GtsSkillPerkPoints->value += 2.0f;
		} else if (level == 60 || level == 80) {
			GtsSkillPerkPoints->value += 3.0f;
		} else if (level == 100) {
			GtsSkillPerkPoints->value += 4.0f;
		}
	}

	void AddStolenAttributes(Actor* giant, float value) {
		if (giant->formID == 0x14 && Runtime::HasPerk(giant, "SizeAbsorption")) {
			auto attributes = Persistent::GetSingleton().GetData(giant);
			if (attributes) {
				attributes->stolen_attributes += value;
				if (attributes->stolen_attributes <= 0.0f) {
					attributes->stolen_attributes = 0.0f; // Cap it just in case
				}
			}
		}
	}

	void AddStolenAttributesTowards(Actor* giant, ActorValue type, float value) {
		if (giant->formID == 0x14) {
			auto Persistent = Persistent::GetSingleton().GetData(giant);
			if (Persistent) {
				float modifier = Runtime::GetFloatOr("LevelLimitModifier", 1.0f);
				float& health = Persistent->stolen_health;
				float& magick = Persistent->stolen_magick;
				float& stamin = Persistent->stolen_stamin;
				float limit = 2.0f * giant->GetLevel() * modifier;
				if (type == ActorValue::kHealth) {
					health += value;
					if (health >= limit) {
						health = limit;
					}
					//log::info("Adding {} to health, health: {}", value, health);
				} else if (type == ActorValue::kMagicka) {
					magick += value;
					if (magick >= limit) {
						magick = limit;
					}
					//log::info("Adding {} to magick, magicka: {}", value, magick);
				} else if (type == ActorValue::kStamina) {
					stamin += value;
					if (stamin >= limit) {
						stamin = limit;
					}
					//log::info("Adding {} to stamina, stamina: {}", value, stamin);
				}
			}
		}
	}

	float GetStolenAttributes_Values(Actor* giant, ActorValue type) {
		if (giant->formID == 0x14) {
			auto Persistent = Persistent::GetSingleton().GetData(giant);
			if (Persistent) {
				if (type == ActorValue::kHealth) {
					return Persistent->stolen_health;
				} else if (type == ActorValue::kMagicka) {
					return Persistent->stolen_magick;
				} else if (type == ActorValue::kStamina) {
					return Persistent->stolen_stamin;
				} else {
					return 0.0f;
				}
			}
			return 0.0f;
		}
		return 0.0f;
	}

	float GetStolenAttributes(Actor* giant) {
		auto persist = Persistent::GetSingleton().GetData(giant);
		if (persist) {
			return persist->stolen_attributes;
		}
		return 0.0f;
	}

	void DistributeStolenAttributes(Actor* giant, float value) {
		if (value > 0 && giant->formID == 0x14 && Runtime::HasPerk(giant, "SizeAbsorption")) { // Permamently increases random AV after shrinking and stuff
			float scale = std::clamp(get_visual_scale(giant), 0.01f, 999999.0f);
			float modifier = Runtime::GetFloatOr("LevelLimitModifier", 1.0f);
			float Storage = GetStolenAttributes(giant);
			float limit = 2.0f * giant->GetLevel() * modifier;


			auto Persistent = Persistent::GetSingleton().GetData(giant);
			if (!Persistent) {
				return;
			}
			//log::info("Adding {} to attributes", value);
			float& health = Persistent->stolen_health;
			float& magick = Persistent->stolen_magick;
			float& stamin = Persistent->stolen_stamin;

			if (Storage > 0.0f) {
				int Boost = RandomInt(0, 3);
				if (Boost == 0) {
					health += (value * 4);
					if (health >= limit) {
						health = limit;
					}
					//log::info("Adding {} to HP, HP {}", value * 4, health);
				} else if (Boost == 1) {
					magick += (value * 4);
					if (magick >= limit) {
						magick = limit;
					}
					//log::info("Adding {} to MP, MP {}", value * 4, magick);
				} else if (Boost >= 2) {
					stamin += (value * 4);
					if (stamin >= limit) {
						stamin = limit;
					}
					//log::info("Adding {} to SP, SP {}", value * 4, stamin);
				}
				AddStolenAttributes(giant, -value); // reduce it
			}
		}
	}

	float GetRandomBoost() {
		float rng = (RandomFloat(0, 150));
		float random = rng/100.f;
		return random;
	}

	float GetButtCrushCost(Actor* actor, bool DoomOnly) {
		float cost = 1.0f;
		if (!DoomOnly && Runtime::HasPerkTeam(actor, "ButtCrush_KillerBooty")) {
			cost -= 0.15f;
		}
		if (Runtime::HasPerkTeam(actor, "ButtCrush_LoomingDoom")) {
			cost -= 0.25f;
		}
		cost *= Perk_GetCostReduction(actor);
		return cost;
	}

	float Perk_GetCostReduction(Actor* giant) {
		float cost = 1.0f;
		float reduction_1 = 0.0f;
		float reduction_2 = 1.0f;
		if (Runtime::HasPerkTeam(giant, "SkilledGTS")) {
			reduction_1 += std::clamp(GetGtsSkillLevel(giant) * 0.0035f, 0.0f, 0.35f);
		}
		if (giant->formID == 0x14 && HasGrowthSpurt(giant)) {
			if (Runtime::HasPerkTeam(giant, "GrowthOfStrength")) {
				reduction_2 -= 0.10f;
			} 
			if (Runtime::HasPerk(giant, "ExtraGrowth")) {
				reduction_2 -= 0.30f;
			}
		}
		cost -= reduction_1;
		cost *= reduction_2;
		cost *= (1.0f - Potion_GetMightBonus(giant));
		cost *= (1.0f - std::clamp(GetGtsSkillLevel(giant) * 0.0020f, 0.0f, 0.20f)); // Based on skill tree progression
		return cost;
	}

	float GetAnimationSlowdown(Actor* giant) {
		if (!giant) {
			return 1.0f;
		}
		if (!Persistent::GetSingleton().is_speed_adjusted) {
			return 1.0f;
		}

		if (giant->AsActorState()->GetSitSleepState() != SIT_SLEEP_STATE::kNormal){
            return 1.0f; // For some reason makes furniture angles funny if there's anim slowdown. So we prevent that
        }

		float scale = get_visual_scale(giant);
		SoftPotential getspeed {
			.k = 0.142f, // 0.125f
			.n = 0.82f, // 0.86f
			.s = 1.90f, // 1.12f
			.o = 1.0f,
			.a = 0.0f,  //Default is 0
		};
		float speedmultcalc = soft_core(scale, getspeed);

		if (speedmultcalc > 1.0f) { // We don't want it to be > 1
			speedmultcalc = 1.0f;
		}

		return speedmultcalc;
	}

	void DoFootstepSound(Actor* giant, float modifier, FootEvent kind, std::string_view node) {
		auto& footstepSound = FootStepManager::GetSingleton();

		std::vector<NiAVObject*> points = {find_node(giant, node)};

		Impact impact_data = Impact {
			.actor = giant,
			.kind = kind,
			.scale = get_visual_scale(giant),
			.modifier = modifier,
			.nodes = points,
		};
		footstepSound.OnImpact(impact_data); // Play sound
	}

	void DoDustExplosion(Actor* giant, float modifier, FootEvent kind, std::string_view node) {
		auto& explosion = ExplosionManager::GetSingleton();

		std::vector<NiAVObject*> points = {find_node(giant, node)};
		
		Impact impact_data = Impact {
			.actor = giant,
			.kind = kind,
			.scale = get_visual_scale(giant),
			.modifier = modifier,
			.nodes = points,
		};
		explosion.OnImpact(impact_data); // Play explosion
	}

	void SpawnParticle(Actor* actor, float lifetime, const char* modelName, const NiMatrix3& rotation, const NiPoint3& position, float scale, std::uint32_t flags, NiAVObject* target) {
		auto cell = actor->GetParentCell();
		if (cell) {
			BSTempEffectParticle::Spawn(cell, lifetime, modelName, rotation, position, scale, flags, target);
		}
	}

	void SpawnDustParticle(Actor* giant, Actor* tiny, std::string_view node, float size) {
		auto result = find_node(giant, node);
		if (result) {
			BGSExplosion* base_explosion = Runtime::GetExplosion("draugrexplosion");
			if (base_explosion) {
				NiPointer<TESObjectREFR> instance_ptr = giant->PlaceObjectAtMe(base_explosion, false);
				if (!instance_ptr) {
					return;
				}
				TESObjectREFR* instance = instance_ptr.get();
				if (!instance) {
					return;
				}
				Explosion* explosion = instance->AsExplosion();
				if (!explosion) {
					return;
				}
				explosion->SetPosition(result->world.translate);
				explosion->GetExplosionRuntimeData().radius *= 3 * get_visual_scale(tiny) * size;
				explosion->GetExplosionRuntimeData().imodRadius *= 3 * get_visual_scale(tiny) * size;
				explosion->GetExplosionRuntimeData().unkB8 = nullptr;
				explosion->GetExplosionRuntimeData().negativeVelocity *= 0.0f;
				explosion->GetExplosionRuntimeData().unk11C *= 0.0f;
			}
		}
	}

	void StaggerOr(Actor* giant, Actor* tiny, float afX, float afY, float afZ, float afMagnitude) {
		if (tiny->IsDead()) {
			return;
		}
		if (InBleedout(tiny)) {
			return;
		}
		if (IsBeingHeld(giant, tiny)) {
			return;
		}
		if (!AllowStagger(giant, tiny)) {
			return;
		} 

		float giantSize = get_visual_scale(giant);
		float tinySize = get_visual_scale(tiny); 

		if (HasSMT(giant)) {
			giantSize += 1.0f;
		} if (tiny->formID == 0x14 && HasSMT(tiny)) {
			tinySize += 1.25f;
		}

		float sizedifference = giantSize/tinySize;
		float sizedifference_tinypov = tinySize/giantSize;

		int ragdollchance = RandomInt(0, 30);
		if ((giantSize > 1.25f || IsBeingGrinded(tiny)) && !IsRagdolled(tiny) && sizedifference > 2.8f && ragdollchance < 4.0f * sizedifference) { // Chance for ragdoll. Becomes 100% at high scales
			PushActorAway(giant, tiny, 1.0f); // Ragdoll
			return;
		} else if (sizedifference > 1.25f) { // Always Stagger
			tiny->SetGraphVariableFloat("GiantessScale", sizedifference_tinypov); // enable stagger just in case

		    float push = std::clamp(0.25f * (sizedifference - 0.25f), 0.25f, 1.0f);
			StaggerActor(giant, tiny, push);
			return;
		}
	}

	void Utils_PushCheck(Actor* giant, Actor* tiny, float force) {
		auto model = tiny->GetCurrent3D();
	
		if (model) {
			bool isdamaging = IsActionOnCooldown(tiny, CooldownSource::Push_Basic);
			if (!isdamaging && (force >= 0.12f || IsFootGrinding(giant))) {
				//log::info("Check passed, pushing {}, force: {}", tiny->GetDisplayFullName(), force);
				StaggerOr(giant, tiny, 0, 0, 0, 0);
				ApplyActionCooldown(tiny, CooldownSource::Push_Basic);
			}
		}
	}

	void DoDamageEffect(Actor* giant, float damage, float radius, int random, float bonedamage, FootEvent kind, float crushmult, DamageSource Cause) {
		DoDamageEffect(giant, damage, radius, random, bonedamage, kind, crushmult, Cause, false);
	}
	void DoDamageEffect(Actor* giant, float damage, float radius, int random, float bonedamage, FootEvent kind, float crushmult, DamageSource Cause, bool ignore_rotation) {
		if (kind == FootEvent::Left) {
			CollisionDamage::GetSingleton().DoFootCollision(giant, damage, radius, random, bonedamage, crushmult, Cause, false, false, ignore_rotation, true);
		}
		if (kind == FootEvent::Right) {
			CollisionDamage::GetSingleton().DoFootCollision(giant, damage, radius, random, bonedamage, crushmult, Cause, true, false, ignore_rotation, true);
			//                                                                                  ^        ^           ^ - - - - Normal Crush
			//                                                       Chance to trigger bone crush   Damage of            Threshold multiplication
			//                                                                                      Bone Crush
		}
	}

	void PushTowards(Actor* giantref, Actor* tinyref, std::string_view bone, float power, bool sizecheck) {
		if (!AllowStagger(giantref, tinyref)) {
			return;
		} 
		NiAVObject* node = find_node(giantref, bone);
		if (node) {
			PushTowards(giantref, tinyref, node, power, sizecheck);
		}
	}

	void PushTowards_Task(ActorHandle giantHandle, ActorHandle tinyHandle, const NiPoint3& startCoords, const NiPoint3& endCoords, std::string_view TaskName, float power, bool sizecheck) {

		float startTime = static_cast<float>(Time::WorldTimeElapsed());

		TaskManager::RunFor(TaskName, 2, [=](auto& update){
			if (!giantHandle) {
				return false;
			}
			if (!tinyHandle) {
				return false;
			}
			Actor* giant = giantHandle.get().get();
			Actor* tiny = tinyHandle.get().get();
			
			float endTime = static_cast<float>(Time::WorldTimeElapsed());
			if (!tiny) {
				return false;
			} 
			if (!tiny->Is3DLoaded()) {
				return true;
			}
			if (!tiny->GetCurrent3D()) {
				return true;
			}
			if ((endTime - startTime) > 0.05) {
				// Enough time has elapsed

				NiPoint3 vector = endCoords - startCoords;
				float distanceTravelled = vector.Length();
				float timeTaken = endTime - startTime;
				float speed = (distanceTravelled / timeTaken) / GetAnimationSlowdown(giant);
				NiPoint3 direction = vector / vector.Length();

				if (sizecheck) {
					float giantscale = get_visual_scale(giant);
					float tinyscale = get_visual_scale(tiny) * GetSizeFromBoundingBox(tiny);

					if (tiny->IsDead()) {
						tinyscale *= 0.4f;
					}

					if (HasSMT(giant)) {
						giantscale *= 6.0f;
					}
					float sizedifference = giantscale/tinyscale;

					if (sizedifference < 1.2f) {
						return false; // terminate task
					} else if (sizedifference > 1.2f && sizedifference < 3.0f) {
						StaggerActor(giant, tiny, 0.25f * sizedifference);
						return false; //Only Stagger
					}
				}

				float Time = (1.0f / Time::GetTimeMultiplier());
				ApplyManualHavokImpulse(tiny, direction.x, direction.y, direction.z, speed * 2.0f * power * Time);

				return false;
			}
			return true;
		});
	}

	void PushTowards(Actor* giantref, Actor* tinyref, NiAVObject* bone, float power, bool sizecheck) {
		NiPoint3 startCoords = bone->world.translate;
		
		ActorHandle tinyHandle = tinyref->CreateRefHandle();
		ActorHandle giantHandle = giantref->CreateRefHandle();

		PushActorAway(giantref, tinyref, 1.0f);

		double startTime = Time::WorldTimeElapsed();

		std::string name = std::format("PushTowards_{}_{}", giantref->formID, tinyref->formID);
		std::string TaskName = std::format("PushTowards_Job_{}_{}", giantref->formID, tinyref->formID);
		// Do this next frame (or rather until some world time has elapsed)
		TaskManager::Run(name, [=](auto& update){
			if (!giantHandle) {
				return false;
			}
			if (!tinyHandle) {
				return false;
			}
			Actor* giant = giantHandle.get().get();

			if (!giant->Is3DLoaded()) {
				return true;
			}
			if (!giant->GetCurrent3D()) {
				return true;
			} 

			double endTime = Time::WorldTimeElapsed();

			if ((endTime - startTime) > 1e-4) {

				NiPoint3 endCoords = bone->world.translate;

				//log::info("Passing coords: Start: {}, End: {}", Vector2Str(startCoords), Vector2Str(endCoords));
				// Because of delayed nature (and because coordinates become constant once we pass them to TaskManager)
				// i don't have any better idea than to do it through task + task, don't kill me
				PushTowards_Task(giantHandle, tinyHandle, startCoords, endCoords, TaskName, power, sizecheck);
				return false;
			}
			return true;
		});
	}

	void PushForward(Actor* giantref, Actor* tinyref, float power) {
		double startTime = Time::WorldTimeElapsed();
		ActorHandle tinyHandle = tinyref->CreateRefHandle();
		ActorHandle gianthandle = giantref->CreateRefHandle();
		std::string taskname = std::format("PushOther_{}", tinyref->formID);
		PushActorAway(giantref, tinyref, 1.0f);
		TaskManager::Run(taskname, [=](auto& update) {
			if (!gianthandle) {
				return false;
			}
			if (!tinyHandle) {
				return false;
			}
			Actor* giant = gianthandle.get().get();
			Actor* tiny = tinyHandle.get().get();
			
			auto playerRotation = giant->GetCurrent3D()->world.rotate;
			RE::NiPoint3 localForwardVector{ 0.f, 1.f, 0.f };
			RE::NiPoint3 globalForwardVector = playerRotation * localForwardVector;

			RE::NiPoint3 direction = globalForwardVector;
			double endTime = Time::WorldTimeElapsed();

			if ((endTime - startTime) > 0.05f) {
				ApplyManualHavokImpulse(tiny, direction.x, direction.y, direction.z, power);
				return false;
			} else {
				return true;
			}
		});
	}

	void TinyCalamityExplosion(Actor* giant, float radius) { // Meant to just stagger actors
		if (!giant) {
			return;
		}
		auto node = find_node(giant, "NPC Root [Root]");
		if (!node) {
			return;
		}
		float giantScale = get_visual_scale(giant);
		NiPoint3 NodePosition = node->world.translate;
		const float maxDistance = radius;
		float totaldistance = maxDistance * giantScale;
		// Make a list of points to check
		if (IsDebugEnabled() && (giant->formID == 0x14 || IsTeammate(giant))) {
			DebugAPI::DrawSphere(glm::vec3(NodePosition.x, NodePosition.y, NodePosition.z), totaldistance, 600, {0.0f, 1.0f, 0.0f, 1.0f});
		}

		NiPoint3 giantLocation = giant->GetPosition();

		for (auto otherActor: find_actors()) {
			if (otherActor != giant) {
				NiPoint3 actorLocation = otherActor->GetPosition();
				if ((actorLocation-giantLocation).Length() < (maxDistance*giantScale * 3.0f)) {
					int nodeCollisions = 0;
					float force = 0.0f;
					auto model = otherActor->GetCurrent3D();
					if (model) {
						VisitNodes(model, [&nodeCollisions, &force, NodePosition, totaldistance](NiAVObject& a_obj) {
							float distance = (NodePosition - a_obj.world.translate).Length();
							if (distance < totaldistance) {
								nodeCollisions += 1;
								force = 1.0f - distance / totaldistance;
								return false;
							}
							return true;
						});
					}
					if (nodeCollisions > 0) {
						float sizedifference = giantScale/get_visual_scale(otherActor);
						if (sizedifference <= 1.6f) {
							StaggerActor(giant, otherActor, 0.75f);
						} else {
							PushActorAway(giant, otherActor, 1.0f * GetLaunchPower(giant, sizedifference));
						}
					}
				}
			}
		}
	}

	void ShrinkOutburst_Shrink(Actor* giant, Actor* tiny, float shrink, float gigantism) {
		if (IsEssential(giant, tiny)) { // Protect followers/essentials
			return;
		}
		bool DarkArts1 = Runtime::HasPerk(giant, "DarkArts_Aug");
		bool DarkArts2 = Runtime::HasPerk(giant, "DarkArts_Aug2");

		float shrinkpower = (shrink * 0.35f) * (1.0f + (GetGtsSkillLevel(giant) * 0.005f)) * CalcEffeciency(giant, tiny);

		float Adjustment = GetSizeFromBoundingBox(tiny);

		float sizedifference = GetSizeDifference(giant, tiny, SizeType::VisualScale, false, false);
		if (DarkArts1) {
			giant->AsActorValueOwner()->RestoreActorValue(ACTOR_VALUE_MODIFIER::kDamage, ActorValue::kHealth, 8.0f);
		}
		if (DarkArts2 && (IsGrowthSpurtActive(giant) || HasSMT(giant))) {
			shrinkpower *= 1.40f;
		}

		update_target_scale(tiny, -(shrinkpower * gigantism), SizeEffectType::kShrink);
		Attacked(tiny, giant);

		ModSizeExperience(giant, (shrinkpower * gigantism) * 0.60f);

		float MinScale = SHRINK_TO_NOTHING_SCALE / Adjustment;

		if (get_target_scale(tiny) <= MinScale) {
			set_target_scale(tiny, MinScale);
		}
		if (!IsBetweenBreasts(tiny)) {
			if (sizedifference <= 4.0f) { // Stagger or Push
				StaggerActor(giant, tiny, 0.25f);
			} else {
				PushActorAway(giant, tiny, 1.0f/Adjustment * GetLaunchPower(giant, sizedifference));
			}
		}
	}

	void ShrinkOutburstExplosion(Actor* giant, bool WasHit) {
		if (!giant) {
			return;
		}
		auto node = find_node(giant, "NPC Pelvis [Pelv]");
		if (!node) {
			return;
		}
		NiPoint3 NodePosition = node->world.translate;

		float giantScale = get_visual_scale(giant);
		float gigantism = 1.0f + Ench_Aspect_GetPower(giant);
		float shrink = 0.38f;
		float radius = 1.0f;

		float explosion = 0.75f;
		bool DarkArts1 = Runtime::HasPerk(giant, "DarkArts_Aug");
		if (WasHit) {
			radius *= 1.4f;
			shrink += 0.20f;
			explosion += 0.95f;
		}
		if (DarkArts1) {
			radius *= 1.33f;
			shrink *= 1.33f;
			explosion += 0.30f;
		}

		const float BASE_DISTANCE = 84.0f;
		float CheckDistance = BASE_DISTANCE*giantScale*gigantism*radius;

		Runtime::PlaySoundAtNode("ShrinkOutburstSound", giant, explosion, 1.0f, "NPC Pelvis [Pelv]");
		Rumbling::For("ShrinkOutburst", giant, Rumble_Misc_ShrinkOutburst, 0.15f, "NPC COM [COM ]", 0.60f, 0.0f);

		SpawnParticle(giant, 6.00f, "GTS/Shouts/ShrinkOutburst.nif", NiMatrix3(), NodePosition, giantScale*explosion*3.0f, 7, nullptr); // Spawn effect

		if (IsDebugEnabled() && (giant->formID == 0x14 || IsTeammate(giant))) {
			DebugAPI::DrawSphere(glm::vec3(NodePosition.x, NodePosition.y, NodePosition.z), CheckDistance, 600, {0.0f, 1.0f, 0.0f, 1.0f});
		}

		NiPoint3 giantLocation = giant->GetPosition();
		for (auto otherActor: find_actors()) {
			if (otherActor != giant) {
				NiPoint3 actorLocation = otherActor->GetPosition();
				if ((actorLocation - giantLocation).Length() < BASE_DISTANCE*giantScale*radius*3) {
					int nodeCollisions = 0;
					float force = 0.0f;

					auto model = otherActor->GetCurrent3D();

					if (model) {
						VisitNodes(model, [&nodeCollisions, &force, NodePosition, CheckDistance](NiAVObject& a_obj) {
							float distance = (NodePosition - a_obj.world.translate).Length();
							if (distance < CheckDistance) {
								nodeCollisions += 1;
								force = 1.0f - distance / CheckDistance;
								return false;
							}
							return true;
						});
					}
					if (nodeCollisions > 0) {
						ShrinkOutburst_Shrink(giant, otherActor, shrink, gigantism);
					}
				}
			}
		}
	}

	void Utils_ProtectTinies(bool Balance) { // This is used to avoid damaging friendly actors in towns and in general
		auto player = PlayerCharacter::GetSingleton();

		for (auto actor: find_actors()) {
			if (actor == player || IsTeammate(actor)) {
				float scale = get_visual_scale(actor);

				SpawnCustomParticle(actor, ParticleType::Red, NiPoint3(), "NPC Root [Root]", scale * 1.15f);
				Runtime::PlaySoundAtNode("Magic_ProtectTinies", actor, 1.0f, 1.0f, "NPC COM [COM ]");

				std::string name_com = std::format("Protect_{}", actor->formID);
				std::string name_root = std::format("Protect_Root_{}", actor->formID);

				Rumbling::Once(name_com, actor, 4.0f, 0.20f, "NPC COM [COM ]", 0.0f);
				Rumbling::Once(name_root, actor, 4.0f, 0.20f, "NPC Root [Root]", 0.0f);
				
				LaunchImmunityTask(actor, Balance);
			}
		}
	}

	void LaunchImmunityTask(Actor* giant, bool Balance) {
		auto transient = Transient::GetSingleton().GetData(giant);
		if (transient) {
			transient->Protection = true;
		}

		std::string name = std::format("Protect_{}", giant->formID);
		std::string name_1 = std::format("Protect_1_{}", giant->formID);

		TaskManager::Cancel(name); // Stop old task if it's been running

		Rumbling::Once(name, giant, Rumble_Misc_EnableTinyProtection, 0.20f, "NPC COM [COM ]", 0.0f);
		Rumbling::Once(name_1, giant, Rumble_Misc_EnableTinyProtection, 0.20f, "NPC Root [Root]", 0.0f);

		float Start = static_cast<float>(Time::WorldTimeElapsed());
		ActorHandle gianthandle = giant->CreateRefHandle();
		TaskManager::Run(name, [=](auto& progressData) {
			if (!gianthandle) {
				return false;
			}

			float Finish = static_cast<float>(Time::WorldTimeElapsed());
			float timepassed = Finish - Start;
			if (timepassed < 180.0f) {
				auto giantref = gianthandle.get().get();
				if (Utils_ManageTinyProtection(giant, false, Balance)) {
					return true; // Disallow to check further
				}
			}
			if (transient) {
				transient->Protection = false; // reset protection to default value
			}
			return Utils_ManageTinyProtection(giant, true, Balance); // stop task, immunity has ended
		});
	}


	bool HasSMT(Actor* giant) {
		if (Runtime::HasMagicEffect(giant, "TinyCalamity")) {
			return true;
		}
		return false;
	}

	void NotifyWithSound(Actor* actor, std::string_view message) {
		if (actor->formID == 0x14 || IsTeammate(actor)) {
			static Timer Cooldown = Timer(1.2f);
			if (Cooldown.ShouldRun()) {
				float falloff = 0.13f * get_visual_scale(actor);
				Runtime::PlaySoundAtNode_FallOff("VoreSound_Fail", actor, 0.4f, 1.0f, "NPC COM [COM ]", falloff);
				Notify(message);
			}
		}
	}

	hkaRagdollInstance* GetRagdoll(Actor* actor) {
		BSAnimationGraphManagerPtr animGraphManager;
		if (actor->GetAnimationGraphManager(animGraphManager)) {
			for (auto& graph : animGraphManager->graphs) {
				if (graph) {
					auto& character = graph->characterInstance;
					auto ragdollDriver = character.ragdollDriver.get();
					if (ragdollDriver) {
						auto ragdoll = ragdollDriver->ragdoll;
						if (ragdoll) {
							return ragdoll;
						}
					}
				}
			}
		}
		return nullptr;
	}

	void DisarmActor(Actor* tiny, bool drop) {
		if (tiny->formID != 0x14) {
			if (!drop) {
				for (auto &object_find : disarm_nodes) {
					auto object = find_node(tiny, object_find);
					if (object) {
						//log::info("Found Object: {}", object->name.c_str());
						//object->local.scale = 0.01f;
						object->SetAppCulled(true);

						update_node(object);
						
						std::string objectname = object->name.c_str();
						std::string name = std::format("ScaleWeapons_{}_{}", tiny->formID, objectname);
						ActorHandle tinyHandle = tiny->CreateRefHandle();

						float Start = static_cast<float>(Time::WorldTimeElapsed());

						TaskManager::Run(name,[=](auto& progressData) {
							if (!tinyHandle) {
								return false;
							}
							Actor* Tiny = tinyHandle.get().get();
							float Finish = static_cast<float>(Time::WorldTimeElapsed());
							if (Finish - Start > 0.25f && !IsGtsBusy(Tiny)) {
								//object->local.scale = 1.0f;
								object->SetAppCulled(false);
								update_node(object);
								return false;
							}

							return true;
						});
					}
				}
			} else {
				NiPoint3 dropPos = tiny->GetPosition();
				for (auto object: {tiny->GetEquippedObject(true), tiny->GetEquippedObject(false)}) {
					dropPos.x += 25 * get_visual_scale(tiny);
					dropPos.y += 25 * get_visual_scale(tiny);
					if (object) {
						log::info("Object found");
						
						TESBoundObject* as_object = skyrim_cast<TESBoundObject*>(object);
						if (as_object) {
							log::info("As object exists, {}", as_object->GetName());
							dropPos.z += 40 * get_visual_scale(tiny);
							tiny->RemoveItem(as_object, 1, ITEM_REMOVE_REASON::kDropping, nullptr, nullptr, &dropPos, nullptr);
						}
					}
				}
			}
		}
	}

	void ManageRagdoll(Actor* tiny, float deltaLength, NiPoint3 deltaLocation, NiPoint3 targetLocation) {
		if (deltaLength >= 70.0f) {
			// WARP if > 1m
			auto ragDoll = GetRagdoll(tiny);
			hkVector4 delta = hkVector4(deltaLocation.x/70.0f, deltaLocation.y/70.0f, deltaLocation.z/70, 1.0f);
			for (auto rb: ragDoll->rigidBodies) {
				if (rb) {
					auto ms = rb->GetMotionState();
					if (ms) {
						hkVector4 currentPos = ms->transform.translation;
						hkVector4 newPos = currentPos + delta;
						rb->motion.SetPosition(newPos);
						rb->motion.SetLinearVelocity(hkVector4(0.0f, 0.0f, -10.0f, 0.0f));
					}
				}
			}
		} else {
			// Just move the hand if <1m
			std::string_view handNodeName = "NPC HAND L [L Hand]";
			auto handBone = find_node(tiny, handNodeName);
			if (handBone) {
				auto collisionHand = handBone->GetCollisionObject();
				if (collisionHand) {
					auto handRbBhk = collisionHand->GetRigidBody();
					if (handRbBhk) {
						auto handRb = static_cast<hkpRigidBody*>(handRbBhk->referencedObject.get());
						if (handRb) {
							auto ms = handRb->GetMotionState();
							if (ms) {
								hkVector4 targetLocationHavok = hkVector4(targetLocation.x/70.0f, targetLocation.y/70.0f, targetLocation.z/70, 1.0f);
								handRb->motion.SetPosition(targetLocationHavok);
								handRb->motion.SetLinearVelocity(hkVector4(0.0f, 0.0f, -10.0f, 0.0f));
							}
						}
					}
				}
			}
		}
	}

	void ChanceToScare(Actor* giant, Actor* tiny, float duration, int random, bool apply_sd) {
		if (tiny->formID == 0x14 || IsTeammate(tiny)) {
			return;
		}
		float sizedifference = GetSizeDifference(giant, tiny, SizeType::VisualScale, true, true);
		if (sizedifference > 1.15f && !tiny->IsDead()) {
			int rng = RandomInt(0, random);
			if (apply_sd) {
				rng = static_cast<int>(static_cast<float>(rng) / sizedifference);
			}
			if (rng <= 1.0f * sizedifference) {
				bool IsScared = IsActionOnCooldown(tiny, CooldownSource::Action_ScareOther);
				if (!IsScared && GetAV(tiny, ActorValue::kConfidence) > 0) {
					ApplyActionCooldown(tiny, CooldownSource::Action_ScareOther);
					ForceFlee(giant, tiny, duration, true); // always scare
				}
			}
		}
	}

	void StaggerActor(Actor* receiver, float power) {
		if (receiver->IsDead() || IsRagdolled(receiver) || GetAV(receiver, ActorValue::kHealth) <= 0.0f) {
			return;
		}
		receiver->SetGraphVariableFloat("staggerMagnitude", power);
		receiver->NotifyAnimationGraph("staggerStart");
	}

	void StaggerActor(Actor* giant, Actor* tiny, float power) {
		if (tiny->IsDead() || IsRagdolled(tiny) || IsBeingHugged(tiny) || GetAV(tiny, ActorValue::kHealth) <= 0.0f) {
			return;
		}
		StaggerActor_Directional(giant, power, tiny);
	}

	void StaggerActor_Around(Actor* giant, const float radius, bool launch) {
		if (!giant) {
			return;
		}
		auto node = find_node(giant, "NPC Root [Root]");
		if (!node) {
			return;
		}
		NiPoint3 NodePosition = node->world.translate;

		float giantScale = get_visual_scale(giant) * GetSizeFromBoundingBox(giant);
		float CheckDistance = radius * giantScale;

		if (IsDebugEnabled() && (giant->formID == 0x14 || IsTeammate(giant))) {
			DebugAPI::DrawSphere(glm::vec3(NodePosition.x, NodePosition.y, NodePosition.z), CheckDistance, 600, {0.0f, 1.0f, 0.0f, 1.0f});
		}

		NiPoint3 giantLocation = giant->GetPosition();
		for (auto otherActor: find_actors()) {
			if (otherActor != giant) {
				NiPoint3 actorLocation = otherActor->GetPosition();
				if ((actorLocation - giantLocation).Length() < CheckDistance*3) {
					int nodeCollisions = 0;
					float force = 0.0f;

					auto model = otherActor->GetCurrent3D();

					if (model) {
						VisitNodes(model, [&nodeCollisions, &force, NodePosition, CheckDistance](NiAVObject& a_obj) {
							float distance = (NodePosition - a_obj.world.translate).Length();
							if (distance < CheckDistance) {
								nodeCollisions += 1;
								force = 1.0f - distance / CheckDistance;
								return false;
							}
							return true;
						});
					}
					if (nodeCollisions > 0) {
						if (!launch) {
							StaggerActor(giant, otherActor, 0.50f);
						} else {
							if (GetSizeDifference(giant, otherActor, SizeType::VisualScale, true, false) < Push_Jump_Launch_Threshold) {
								StaggerActor(giant, otherActor, 0.50f);
							} else {
								float launch_power = 0.33f;
								if (HasSMT(giant)) {
									launch_power *= 6.0f;
								}
								LaunchActor::ApplyLaunchTo(giant, otherActor, 1.0f, launch_power);
							}
						}
					}
				}
			}
		}
	}

	float GetMovementModifier(Actor* giant) {
		float modifier = 1.0f;
		if (giant->AsActorState()->IsSprinting()) {
			modifier *= 1.33f;
		}
		if (giant->AsActorState()->IsWalking()) {
			modifier *= 0.75f;
		}
		if (giant->AsActorState()->IsSneaking()) {
			modifier *= 0.75f;
		}
		return modifier;
	}

	float GetGtsSkillLevel(Actor* giant) {
		if (giant->formID == 0x14) {
			auto GtsSkillLevel = Runtime::GetGlobal("GtsSkillLevel");
			return GtsSkillLevel->value;
		} else {
			float Alteration = std::clamp(GetAV(giant, ActorValue::kAlteration), 1.0f, 100.0f);
			return Alteration;
		}
	}

	float GetXpBonus() {
		float xp = Persistent::GetSingleton().experience_mult;
		return xp;
	}

	void DragonAbsorptionBonuses() { // The function is ugly but im a bit lazy to make it look pretty
		int rng = RandomInt(0, 6);
		int dur_rng = RandomInt(0, 3);

		Actor* player = PlayerCharacter::GetSingleton();

		if (!Runtime::HasPerk(player, "MightOfDragons")) {
			return;
		}

		TESGlobal* BonusSize = Runtime::GetGlobal("ExtraPotionSize"); 

		ModSizeExperience(player, 0.45f);

		if (BonusSize) {
			Notify("You feel like something is filling you");
			BonusSize->value += 0.0659f; // +12 cm

			if (rng <= 1) {
				PlayMoanSound(player, 1.0f);
				Task_FacialEmotionTask_Moan(player, 1.6f, "DragonVored");
				shake_camera(player, 0.5f, 0.33f);
			}

			SpawnCustomParticle(player, ParticleType::Red, NiPoint3(), "NPC COM [COM ]", get_visual_scale(player) * 1.6f); 
			
			ActorHandle gianthandle = player->CreateRefHandle();
			std::string name = std::format("DragonGrowth_{}", player->formID);

			float HpRegen = GetMaxAV(player, ActorValue::kHealth) * 0.00125f;
			float Gigantism = 1.0f + Ench_Aspect_GetPower(player);

			float duration = 6.0f + dur_rng;

			TaskManager::RunFor(name, duration, [=](auto& progressData) {
				if (!gianthandle) {
					return false;
				}
				auto giantref = gianthandle.get().get();
				ApplyShakeAtNode(giantref, Rumble_Misc_MightOfDragons, "NPC COM [COM ]");
				update_target_scale(giantref, 0.0026f * Gigantism * TimeScale(), SizeEffectType::kGrow);
				giantref->AsActorValueOwner()->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, ActorValue::kHealth, HpRegen * TimeScale());
				return true;
			});
		}
	}

	void AddSMTDuration(Actor* actor, float duration, bool perk_check) {
		if (!HasSMT(actor)) {
			return;
		}
		if (!perk_check || Runtime::HasPerk(actor, "EternalCalamity")) {
			auto transient = Transient::GetSingleton().GetData(actor);
			if (transient) {
				transient->SMT_Bonus_Duration += duration;
				log::info("Adding perk duration");
			}
		}
	}

	void AddSMTPenalty(Actor* actor, float penalty) {
		auto transient = Transient::GetSingleton().GetData(actor);
		if (transient) {
			transient->SMT_Penalty_Duration += penalty;
		}
	}

	void PrintDeathSource(Actor* giant, Actor* tiny, DamageSource cause) {
		ReportDeath(giant, tiny, cause);
	}

	void PrintSuffocate(Actor* pred, Actor* prey) {
		int random = RandomInt(0, 5);
		if (random <= 1) {
			Cprint("{} was slowly smothered between {} thighs", prey->GetDisplayFullName(), pred->GetDisplayFullName());
		} else if (random == 2) {
			Cprint("{} was suffocated by the thighs of {}", prey->GetDisplayFullName(), pred->GetDisplayFullName());
		} else if (random == 3) {
			Cprint("Thighs of {} suffocated {} to death", pred->GetDisplayFullName(), prey->GetDisplayFullName());
		} else if (random == 4) {
			Cprint("{} got smothered between the thighs of {}", prey->GetDisplayFullName(), pred->GetDisplayFullName());
		} else if (random >= 5) {
			Cprint("{} lost life to the thighs of {}", prey->GetDisplayFullName(), pred->GetDisplayFullName());
		}
	}

	void ShrinkUntil(Actor* giant, Actor* tiny, float expected, float halflife, bool animation) {
		if (HasSMT(giant)) {
			float Adjustment_Gts = GetSizeFromBoundingBox(giant);
			float Adjustment_Tiny = GetSizeFromBoundingBox(tiny);
			float predscale = get_visual_scale(giant) * Adjustment_Gts;
			float preyscale = get_target_scale(tiny) * Adjustment_Tiny;
			float targetScale = predscale/(expected * Adjustment_Tiny);

			/*log::info("Trying to Shrink {}", tiny->GetDisplayFullName());
			log::info("----Adjustment: GTS: {}", Adjustment_Gts);
			log::info("----Adjustment: Tiny: {}", Adjustment_Tiny);
			log::info("----Pred scale: {}", predscale);
			log::info("----Prey scale: {}", preyscale);
			log::info("----Targeted Scale: {}", targetScale);
			log::info("----Get Target Scale: {}", get_target_scale(tiny));
			*/

			if (preyscale > targetScale) { // Apply ONLY if target is bigger than requirement

				if (animation) {
					Animation_TinyCalamity::AddToData(giant, tiny, expected);
					AnimationManager::StartAnim("Calamity_ShrinkOther", giant);
					tiny->StopMoving(1.2f);
					return;
				}

				Task_AdjustHalfLifeTask(tiny, halflife, 1.2); // to make them shrink faster
				AddSMTPenalty(giant, 5.0f * Adjustment_Tiny);
				set_target_scale(tiny, targetScale);
				StartCombat(tiny, giant);
				
			}
		}
	}

	void DisableCollisions(Actor* actor, TESObjectREFR* otherActor) {
		if (actor) {
			auto trans = Transient::GetSingleton().GetData(actor);
			if (trans) {
				trans->disable_collision_with = otherActor;
				auto colliders = ActorCollisionData(actor);
				colliders.UpdateCollisionFilter();
				if (otherActor) {
					Actor* asOtherActor = skyrim_cast<Actor*>(otherActor);
					auto otherColliders = ActorCollisionData(asOtherActor);
					otherColliders.UpdateCollisionFilter();
				}
			}
		}
	}
	void EnableCollisions(Actor* actor) {
		if (actor) {
			auto trans = Transient::GetSingleton().GetData(actor);
			if (trans) {
				auto otherActor = trans->disable_collision_with;
				trans->disable_collision_with = nullptr;
				auto colliders = ActorCollisionData(actor);
				colliders.UpdateCollisionFilter();
				if (otherActor) {
					Actor* asOtherActor = skyrim_cast<Actor*>(otherActor);
					auto otherColliders = ActorCollisionData(asOtherActor);
					otherColliders.UpdateCollisionFilter();
				}
			}
		}
	}

	void SpringGrow(Actor* actor, float amt, float halfLife, std::string_view naming, bool drain) {
		if (!actor) {
			return;
		}

		auto growData = std::make_shared<SpringGrowData>(actor, amt, halfLife);
		std::string name = std::format("SpringGrow_{}_{}", naming, actor->formID);
		const float DURATION = halfLife * 3.2f;
		growData->drain = drain;

		TaskManager::RunFor(DURATION,
		                    [ growData ](const auto& progressData) {
			float totalScaleToAdd = growData->amount.value;
			float prevScaleAdded = growData->addedSoFar;
			float deltaScale = totalScaleToAdd - prevScaleAdded;
			bool drain_stamina = growData->drain;
			Actor* actor = growData->actor.get().get();

			if (actor) {
				if (drain_stamina) {
					float stamina = std::clamp(GetStaminaPercentage(actor), 0.05f, 1.0f);
					DamageAV(actor, ActorValue::kStamina, 0.55f * (get_visual_scale(actor) * 0.5f + 0.5f) * stamina * TimeScale());
				}
				auto actorData = Persistent::GetSingleton().GetData(actor);
				if (actorData) {
					float scale = get_target_scale(actor);
					float max_scale = get_max_scale(actor);// * get_natural_scale(actor);
					if (scale < max_scale) {
						if (!drain_stamina) { // Apply only to growth with animation
							actorData->visual_scale += deltaScale;
						}
						actorData->target_scale += deltaScale;
						growData->addedSoFar = totalScaleToAdd;
					}
				}
			}
			return fabs(growData->amount.value - growData->amount.target) > 1e-4;
		});
	}

	void SpringShrink(Actor* actor, float amt, float halfLife, std::string_view naming) {
		if (!actor) {
			return;
		}

		auto growData = std::make_shared<SpringShrinkData>(actor, amt, halfLife);
		std::string name = std::format("SpringShrink_{}_{}", naming, actor->formID);
		const float DURATION = halfLife * 3.2f;
		TaskManager::RunFor(DURATION,
		                    [ growData ](const auto& progressData) {
			float totalScaleToAdd = growData->amount.value;
			float prevScaleAdded = growData->addedSoFar;
			float deltaScale = totalScaleToAdd - prevScaleAdded;
			Actor* actor = growData->actor.get().get();

			if (actor) {
				float stamina = std::clamp(GetStaminaPercentage(actor), 0.05f, 1.0f);
				DamageAV(actor, ActorValue::kStamina, 0.35f * (get_visual_scale(actor) * 0.5f + 0.5f) * stamina * TimeScale());
				auto actorData = Persistent::GetSingleton().GetData(actor);
				if (actorData) {
					actorData->target_scale += deltaScale;
					actorData->visual_scale += deltaScale;
					growData->addedSoFar = totalScaleToAdd;
				}
			}

			return fabs(growData->amount.value - growData->amount.target) > 1e-4;
		});
	}

	void ResetGrab(Actor* giant) {
		if (giant->formID == 0x14 || IsTeammate(giant)) {
			Grab::ExitGrabState(giant);

			giant->SetGraphVariableInt("GTS_GrabbedTiny", 0); // Tell behaviors 'we have nothing in our hands'. A must.
			giant->SetGraphVariableInt("GTS_Grab_State", 0);
			giant->SetGraphVariableInt("GTS_Storing_Tiny", 0);
		}
	}

	void FixAnimationsAndCamera() { // Fixes Animations for GTS Grab Actions and resets the bone tracking on camera
		auto profiler = Profilers::Profile("Utils: Actor State Fix");

		ResetCameraTracking(); // fix the camera tracking if loading previous save while voring/thigh crushing for example

		for (auto giant: find_actors()) {
			if (!giant) {
				continue;
			}
			int StateID;
			int GTSStateID;

			giant->GetGraphVariableInt("currentDefaultState", StateID);
			giant->GetGraphVariableInt("GTS_Def_State", GTSStateID);

			ResetGrab(giant);
			if (GTSStateID != StateID) {
				giant->SetGraphVariableInt("GTS_Def_State", StateID);
			}
		}
	}


	bool CanPerformAnimation(Actor* giant, float type) { // Needed for smooth animation unlocks during quest progression
		// 0 = Hugs
		// 1 = stomps and kicks
		// 2 = Grab and Sandwich
		// 3 = Vore
		// 5 = Others
		if (giant->formID != 0x14) {
			return true;
		} else {
			auto progressionQuest = Runtime::GetQuest("MainQuest");
			if (progressionQuest) {
				auto queststage = progressionQuest->GetCurrentStageID();
				if (queststage >= 10 && type == 0) {
					return true; // allow hugs
				} else if (queststage >= 30 && type == 1) {
					return true; // allow stomps and kicks
				} else if (queststage >= 50 && type == 2) {
					return true; // Allow grabbing and sandwiching
				} else if (queststage >= 60 && type >= 3) {
					return true; // Allow Vore
				} else if (queststage >= 100 && type >= 4) { 
					return true; // When quest is completed
				}
				else {
					return false;
				}
			}
			return false;
		}
	}

	void AdvanceQuestProgression(Actor* giant, Actor* tiny, QuestStage stage, float value, bool vore) {
		if (giant->formID == 0x14) { // Player Only
			auto progressionQuest = Runtime::GetQuest("MainQuest");
			if (progressionQuest) {
				auto queststage = progressionQuest->GetCurrentStageID();
				if (queststage >= 100 || queststage < 10) {
					return;
				}
				switch (stage) {
					case QuestStage::HugSteal: 				// Stage 0: hug steal 2 meters of size
						Persistent::GetSingleton().HugStealCount += value;
					break;
					case QuestStage::HugSpellSteal:			// Stage 1: hug/spell steal 5 meters of size
						if (queststage == 20) {
							Persistent::GetSingleton().StolenSize += value;
						}
					break;
					case QuestStage::Crushing:				// Stage 2: Crush 3 (*4 if dead) enemies
						if (queststage >= 30 && queststage <= 40) {
							Persistent::GetSingleton().CrushCount += value;
							if (value < 1) {
								SpawnCustomParticle(tiny, ParticleType::DarkRed, NiPoint3(), "NPC Root [Root]", 1.0f);
							} else {
								SpawnCustomParticle(tiny, ParticleType::Red, NiPoint3(), "NPC Root [Root]", 1.0f);
							}
							
							float progression = GetQuestProgression(static_cast<int>(QuestStage::Crushing));
							float goal = 3.0f;
							if (queststage == 40) { // Print this if in STN stage
								progression = GetQuestProgression(static_cast<int>(QuestStage::ShrinkToNothing));
								goal = 6.0f;
							}
							Notify("Progress: {:.1f}/{:.1f}", progression, goal);
						}
					break;
					case QuestStage::ShrinkToNothing:		// Stage 3: Crush or Shrink to nothing 6 enemies in total
						if (queststage == 40) {
							Persistent::GetSingleton().STNCount += value;
							if (value < 1) {
								SpawnCustomParticle(tiny, ParticleType::DarkRed, NiPoint3(), "NPC Root [Root]", 1.0f);
							} else {
								SpawnCustomParticle(tiny, ParticleType::Red, NiPoint3(), "NPC Root [Root]", 1.0f);
							}
							Notify("Progress: {:.1f}/{:.1f}", GetQuestProgression(static_cast<int>(QuestStage::ShrinkToNothing)), 6.0f);
						}
					break;
					case QuestStage::HandCrush:				// Stage 4: hand crush 3 enemies
						Persistent::GetSingleton().HandCrushed += value;
						SpawnCustomParticle(tiny, ParticleType::Red, NiPoint3(), "NPC Root [Root]", 1.0f);
						Notify("Progress: {:.1f}/{:.1f}", GetQuestProgression(static_cast<int>(QuestStage::HandCrush)), 3.0f);
					break;
					case QuestStage::Vore:					// Stage 5: Vore 6 enemies
						Persistent::GetSingleton().VoreCount += value;
						SpawnCustomParticle(tiny, ParticleType::Blue, NiPoint3(), "NPC Root [Root]", 1.0f);
						Notify("Progress: {:.1f}/{:.1f}", GetQuestProgression(static_cast<int>(QuestStage::Vore)), 6.0f);
					break;
					case QuestStage::Giant:					// Stage 6: Vore/crush/shrink a Giant
						Persistent::GetSingleton().GiantCount += value;
						if (vore) {
							SpawnCustomParticle(tiny, ParticleType::Blue, NiPoint3(), "NPC Root [Root]", 1.0f);
						} else {
							SpawnCustomParticle(tiny, ParticleType::Red, NiPoint3(), "NPC Root [Root]", 1.0f);
						}
					break;
				}
			}
		}
	}

	float GetQuestProgression(int stage) {
		QuestStage Stage = static_cast<QuestStage>(stage);
		switch (Stage) {
			case QuestStage::HugSteal: 				// Stage 0: hug steal 2 meters of size
				return Persistent::GetSingleton().HugStealCount;
			case QuestStage::HugSpellSteal: 		// Stage 1: hug/spell steal 5 meters of size
				return Persistent::GetSingleton().StolenSize;
			case QuestStage::Crushing: 				// Stage 2: Crush 3 (*4 if dead) enemies
				return Persistent::GetSingleton().CrushCount;
			case QuestStage::ShrinkToNothing:  		// Stage 3: Crush or Shrink to nothing 6 enemies in total
				return (Persistent::GetSingleton().CrushCount - 3.0f) + Persistent::GetSingleton().STNCount;
			case QuestStage::HandCrush: 			// Stage 4: hand crush 3 enemies
				return Persistent::GetSingleton().HandCrushed;
			case QuestStage::Vore: 					// Stage 5: Vore 6 enemies
				return Persistent::GetSingleton().VoreCount;
			case QuestStage::Giant:					// Stage 6: Vore/crush/shrink a Giant
				return Persistent::GetSingleton().GiantCount;
			break;
		}
		return 0.0f;
	}

	void ResetQuest() {
		Persistent::GetSingleton().HugStealCount = 0.0f;
		Persistent::GetSingleton().StolenSize = 0.0f;
		Persistent::GetSingleton().CrushCount = 0.0f;
		Persistent::GetSingleton().STNCount = 0.0f;
		Persistent::GetSingleton().HandCrushed = 0.0f;
		Persistent::GetSingleton().VoreCount = 0.0f;
		Persistent::GetSingleton().GiantCount = 0.0f;
	}

	void SpawnHearts(Actor* giant, Actor* tiny, float Z, float scale, bool hugs) {
		bool Allow = Persistent::GetSingleton().HeartEffects;
		if (Allow) {
			NiPoint3 Position = GetHeartPosition(giant, tiny, hugs);

			if (Z > 0) {
				Position.z -= Z * get_visual_scale(giant);
			}
			
			SpawnCustomParticle(giant, ParticleType::Hearts, Position, "NPC COM [COM ]", get_visual_scale(giant) * scale);
		}
    }

	void SpawnCustomParticle(Actor* actor, ParticleType Type, NiPoint3 spawn_at_point, std::string_view spawn_at_node, float scale_mult) {
		if (actor) {
			float scale = scale_mult * GetSizeFromBoundingBox(actor);

			if (actor->IsDead()) {
				scale *= 0.33f;
			}

			auto node = find_node(actor, spawn_at_node);
			if (!node) {
				return;
			}
			const char* particle_path = "None";
			//log::info("Spawning particle");
			switch (Type) {
				case ParticleType::Red: 
					particle_path = "GTS/Magic/Life_Drain.nif";
				break;
				case ParticleType::DarkRed:
					particle_path = "GTS/Magic/Life_Drain_Dark.nif";
				break;
				case ParticleType::Green:
					particle_path = "GTS/Magic/Slow_Grow.nif";
				break;
				case ParticleType::Blue:
					particle_path = "GTS/Magic/Soul_Drain.nif";
				break;
				case ParticleType::Hearts:
					particle_path = "GTS/Magic/Hearts.nif";
				break;
				case ParticleType::None:
					return;
				break;
			}
			NiPoint3 pos = NiPoint3(); // Empty point 3

			if (spawn_at_point.Length() > 0.01f) { // fill it up
				pos = spawn_at_point;
			} else {
				pos = node->world.translate;
			}

			SpawnParticle(actor, 4.60f, particle_path, NiMatrix3(), pos, scale, 7, nullptr);
		}
	}
	
	void InflictSizeDamage(Actor* attacker, Actor* receiver, float value) {
		if (!receiver->IsDead()) {
			float HpPercentage = GetHealthPercentage(receiver);
			float difficulty = 2.0f; // taking Legendary Difficulty as a base
			float levelbonus = 1.0f + ((GetGtsSkillLevel(attacker) * 0.01f) * 0.50f);
			value *= levelbonus;

			if (receiver->formID != 0x14) { // Mostly a warning to indicate that actor dislikes it (They don't always aggro right away, with mods at least)
				if (value >= GetAV(receiver, ActorValue::kHealth) * 0.50f || HpPercentage < 0.70f) { // in that case make hostile
					if (!IsTeammate(receiver) && !IsHostile(attacker, receiver)) {
						StartCombat(receiver, attacker); // Make actor hostile and add bounty of 40 (can't be configured, needs different hook probably). 
					}
				}
				if (value > 1.0f) { // To prevent aggro when briefly colliding
					Attacked(receiver, attacker);
				}
			} 
			
			ApplyDamage(attacker, receiver, value * difficulty * GetDamageSetting());
		} else if (receiver->IsDead()) {
			Task_InitHavokTask(receiver);
			// ^ Needed to fix this issue:
			//   https://www.reddit.com/r/skyrimmods/comments/402b69/help_looking_for_a_bugfix_dead_enemies_running_in/
		}

	}

	float Sound_GetFallOff(NiAVObject* source, float mult) {
		if (source) {
			float distance_to_camera = unit_to_meter(get_distance_to_camera(source));
			// Camera distance based volume falloff
			return soft_core(distance_to_camera, 0.024f / mult, 2.0f, 0.8f, 0.0f, 0.0f);
		}
		return 1.0f;
	} 

	// RE Fun
	void SetCriticalStage(Actor* actor, int stage) {
		if (stage < 5 && stage >= 0) {
			typedef void (*DefSetCriticalStage)(Actor* actor, int stage);
			REL::Relocation<DefSetCriticalStage> SkyrimSetCriticalStage{ RELOCATION_ID(36607, 37615) };
			SkyrimSetCriticalStage(actor, stage);
		}
	}
	void Attacked(Actor* victim, Actor* agressor) {
		typedef void (*DefAttacked)(Actor* victim, Actor* agressor);
		REL::Relocation<DefAttacked> SkyrimAttacked{ RELOCATION_ID(37672, 38626) }; // 6285A0 (SE) ; 64EE60 (AE)
		SkyrimAttacked(victim, agressor); 
	}

	void StartCombat(Actor* victim, Actor* agressor) {
		// This function starts combat and adds bounty. Sadly adds 40 bounty since it's not a murder, needs other hook for murder bounty.
		typedef void (*DefStartCombat)(Actor* act_1, Actor* act_2, Actor* act_3, Actor* act_4);
		REL::Relocation<DefStartCombat> SkyrimStartCombat{ RELOCATION_ID(36430, 37425) }; // sub_1405DE870 : 36430  (SE) ; 1406050c0 : 37425 (AE)
		SkyrimStartCombat(victim, agressor, agressor, agressor);                          // Called from Attacked above at some point
	}

	void ApplyDamage(Actor* giant, Actor* tiny, float damage) { // applies correct amount of damage and kills actors properly
		typedef void (*DefApplyDamage)(Actor* a_this, float dmg, Actor* aggressor, HitData* maybe_hitdata, TESObjectREFR* damageSrc);
		REL::Relocation<DefApplyDamage> Skyrim_ApplyDamage{ RELOCATION_ID(36345, 37335) }; // 5D6300 (SE)
		Skyrim_ApplyDamage(tiny, damage, nullptr, nullptr, nullptr);
	}

	void SetObjectRotation_X(TESObjectREFR* ref, float X) {
		typedef void (*DefSetRotX)(TESObjectREFR* ref, float rotation);
		REL::Relocation<DefSetRotX> SetObjectRotation_X{ RELOCATION_ID(19360, 19360) }; // 140296680 (SE)
		SetObjectRotation_X(ref, X);
	}

	void StaggerActor_Directional(Actor* giant, float power, Actor* tiny) {
		//SE: 1405FA1B0 : 36700 (Character *param_1,float param_2,Actor *param_3)
		//AE: 140621d80 : 37710
		//log::info("Performing Directional Stagger");
		//log::info("Giant: {}, Tiny: {}, Power: {}", giant->GetDisplayFullName(), tiny->GetDisplayFullName(), power);
		typedef void (*DefStaggerActor_Directional)(Actor* tiny, float power, Actor* giant);
		REL::Relocation<DefStaggerActor_Directional> StaggerActor_Directional{ RELOCATION_ID(36700, 37710) };
		StaggerActor_Directional(tiny, power, giant);
	}

	void SetLinearImpulse(bhkRigidBody* body, const hkVector4& a_impulse)
	{
		using func_t = decltype(&SetLinearImpulse);
		REL::Relocation<func_t> func{ RELOCATION_ID(76261, 78091) };
		return func(body, a_impulse);
	}

	void SetAngularImpulse(bhkRigidBody* body, const hkVector4& a_impulse)
	{
		using func_t = decltype(&SetAngularImpulse);
		REL::Relocation<func_t> func{ RELOCATION_ID(76262, 78092) };
		return func(body, a_impulse);
	}

	std::int16_t GetItemCount(InventoryChanges* changes, RE::TESBoundObject* a_obj)
	{
		using func_t = decltype(&GetItemCount);
		REL::Relocation<func_t> func{ RELOCATION_ID(15868, 16108) };
		return func(changes, a_obj);
	}

	int GetCombatState(Actor* actor) { // Needs AE address
        using func_t = decltype(GetCombatState);
        REL::Relocation<func_t> func{ RELOCATION_ID(37603, 37603) };
        return func(actor);
		// 0 = non combat, 1 = combat, 2 = search
    }

	bool IsMoving(Actor* giant) { // New CommonLib version copy-paste
		using func_t = decltype(&IsMoving);
		REL::Relocation<func_t> func{ RELOCATION_ID(36928, 37953) };
		return func(giant);
	}

	//void ForEachReferenceInRange_Custom(RE::TESObjectREFR* origin, float radius, std::function<RE::BSContainer::ForEachResult(RE::TESObjectREFR& ref)> callback) {
	//	if (REL::Module::IsAE()) { // Since commonlib didn't fix this function crashing on AE, we have to create fixed function ourselves
	//		if (origin && radius > 0.0f) {
	//			const auto originPos = origin->GetPosition();
	//			auto* tesSingleton = RE::TES::GetSingleton();
	//			auto* interiorCell = tesSingleton->interiorCell;
	//			if (interiorCell) {
	//				interiorCell->ForEachReferenceInRange(originPos, radius,[&](RE::TESObjectREFR& a_ref) { return callback(a_ref); });
	//			} else {
	//				if (const auto gridLength = tesSingleton->gridCells ? tesSingleton->gridCells->length : 0;
	//					gridLength > 0) {
	//					const float yPlus = originPos.y + radius;
	//					const float yMinus = originPos.y - radius;
	//					const float xPlus = originPos.x + radius;
	//					const float xMinus = originPos.x - radius;

	//					std::uint32_t x = 0;
	//					do {
	//						std::uint32_t y = 0;
	//						do {
	//							if (const auto cell = tesSingleton->gridCells->GetCell(x, y); cell && cell->IsAttached()) {
	//								if (const auto cellCoords = cell->GetCoordinates(); cellCoords) {
	//									const RE::NiPoint2 worldPos{cellCoords->worldX, cellCoords->worldY};
	//									if (worldPos.x < xPlus && (worldPos.x + 4096.0f) > xMinus && worldPos.y < yPlus &&
	//										(worldPos.y + 4096.0f) > yMinus) {
	//										cell->ForEachReferenceInRange(originPos, radius, [&](RE::TESObjectREFR& a_ref) {
	//											return callback(a_ref);
	//										});
	//									}
	//								}
	//							}
	//							++y;
	//						} while (y < gridLength);
	//						++x;
	//					} while (x < gridLength);
	//				}
	//			}
	//		} 
	//	} else { // If on SE, just use old function
	//		TES::GetSingleton()->ForEachReference([&](RE::TESObjectREFR& a_ref) { return callback(a_ref); });
	//	}
	//}
}
