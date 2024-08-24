#include "managers/animation/Utils/AnimationUtils.hpp"
#include "managers/animation/AnimationManager.hpp"
#include "managers/GrabAnimationController.hpp"
#include "managers/emotions/EmotionManager.hpp"
#include "managers/ShrinkToNothingManager.hpp"
#include "managers/damage/CollisionDamage.hpp"
#include "managers/damage/SizeHitEffects.hpp"
#include "managers/damage/TinyCalamity.hpp"
#include "managers/damage/LaunchActor.hpp"
#include "managers/animation/Grab.hpp"
#include "managers/GtsSizeManager.hpp"
#include "managers/ai/aifunctions.hpp"
#include "managers/CrushManager.hpp"
#include "managers/InputManager.hpp"
#include "magic/effects/common.hpp"
#include "managers/Attributes.hpp"
#include "utils/actorUtils.hpp"
#include "data/persistent.hpp"
#include "managers/tremor.hpp"
#include "managers/Rumble.hpp"
#include "data/transient.hpp"
#include "ActionSettings.hpp"
#include "managers/vore.hpp"
#include "data/runtime.hpp"
#include "scale/scale.hpp"
#include "data/time.hpp"
#include "events.hpp"
#include "timer.hpp"
#include "node.hpp"

#include <random>

using namespace RE;
using namespace REL;
using namespace Gts;
using namespace std;

///GTS_GrabbedTiny MUST BE 1 when we have someone in hands

/*Event used in the behaviours to transition between most behaviour states
   Grab Events
        GTSBEH_GrabStart
        GTSBEH_GrabVore
        GTSBEH_GrabAttack
        GTSBEH_GrabThrow
        GTSBEH_GrabRelease
   More Grab things we don't need to do anything with in the DLL
        GTSBeh_MT
        GTSBeh_1hm
        GTSBeh_Mag
        GTSBeh_Set
        GTSBeh_GrabVore_LegTrans
   Used to leave the grab state
        GTSBeh_GrabExit
   Grab Event to go back to vanilla
        GTSBEH_AbortGrab
 */


namespace {

	const std::vector<std::string_view> RHAND_RUMBLE_NODES = { // used for hand rumble
		"NPC R UpperarmTwist1 [RUt1]",
		"NPC R UpperarmTwist2 [RUt2]",
		"NPC R Forearm [RLar]",
		"NPC R ForearmTwist2 [RLt2]",
		"NPC R ForearmTwist1 [RLt1]",
		"NPC R Hand [RHnd]",
	};

	const std::vector<std::string_view> LHAND_RUMBLE_NODES = { // used for hand rumble
		"NPC L UpperarmTwist1 [LUt1]",
		"NPC L UpperarmTwist2 [LUt2]",
		"NPC L Forearm [LLar]",
		"NPC L ForearmTwist2 [LLt2]",
		"NPC L ForearmTwist1 [LLt1]",
		"NPC L Hand [LHnd]",
	};

	const std::string_view RNode = "NPC R Foot [Rft ]";
	const std::string_view LNode = "NPC L Foot [Lft ]";

	bool Escaped(Actor* giant, Actor* tiny, float strength) {
		float tiny_chance = ((rand() % 100000) / 100000.0f) * get_visual_scale(tiny);
		float giant_chance = ((rand() % 100000) / 100000.0f) * strength * get_visual_scale(giant);
		return (tiny_chance > giant_chance);
	}

	void ApplyPitchRotation(Actor* actor, float pitch) {
		auto charCont = actor->GetCharController();
		if (!charCont) {
			return;
		}
		charCont->pitchAngle = pitch;
		//log::info("Pitch: {}", charCont->pitchAngle);
	}

	void ApplyRollRotation(Actor* actor, float roll) {
		auto charCont = actor->GetCharController();
		if (!charCont) {
			return;
		}
		charCont->rollAngle = roll;
		//log::info("Roll: {}", charCont->rollAngle);
	}

	void Task_RotateActorToBreastX(Actor* giant, Actor* tiny) {
		std::string name = std::format("RotateActor_{}", giant->formID);
		ActorHandle gianthandle = giant->CreateRefHandle();
		ActorHandle tinyhandle = tiny->CreateRefHandle();
		TaskManager::Run(name, [=](auto& progressData) {
			if (!gianthandle) {
				return false;
			}
			if (!tinyhandle) {
				return false;
			}
			auto giantref = gianthandle.get().get();
			auto tinyref = tinyhandle.get().get();

			float LPosX = 0.0f;
			float LPosY = 0.0f;
			float LPosZ = 0.0f;

			float RPosX = 0.0f;
			float RPosY = 0.0f;
			float RPosZ = 0.0f;

			auto BreastL = find_node(giant, "L Breast01");
			auto BreastR = find_node(giant, "R Breast01");
			if (!BreastL) {
				return false;
			}
			if (!BreastR) {
				return false;
			}

			NiMatrix3 LeftBreastRotation = BreastL->world.rotate; // get breast rotation
			NiMatrix3 RightBreastRotation = BreastR->world.rotate;

			LeftBreastRotation.ToEulerAnglesXYZ(LPosX, LPosY, LPosZ); // fill empty rotation data of breast with proper one
			RightBreastRotation.ToEulerAnglesXYZ(RPosX, RPosY, RPosZ);

			float BreastRotation_X = (LPosX + RPosX) / 2.0;
			float BreastRotation_Y = (LPosY + RPosY) / 2.0;

			ApplyPitchRotation(tinyref, BreastRotation_X);
			//ApplyRollRotation(tinyref, BreastRotation_Y);
			//log::info("Angle of L breast: X: {}, Y: {}, Z: {}", LPosX, LPosY, LPosZ);
			//log::info("Angle of R breast: X: {}, Y: {}, Z: {}", RPosX, RPosY, RPosZ);

			// All good try another frame
			if (!IsBetweenBreasts(tinyref)) {
				ApplyPitchRotation(tinyref, 0.0);
				//ApplyRollRotation(tinyref, 0.0);
				return false; // Abort it
			}
			return true;
		});
		TaskManager::ChangeUpdate(name, UpdateKind::Havok);
	}

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	/////////////////////////G R A B
	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	void GTSGrab_Catch_Start(AnimationEventData& data) {
		ManageCamera(&data.giant, true, CameraTracking::Grab_Left);
		auto grabbedActor = Grab::GetHeldActor(&data.giant);
		if (grabbedActor) {
			DisableCollisions(grabbedActor, &data.giant);
			SetBeingHeld(grabbedActor, true);

			StaggerActor(&data.giant, grabbedActor, 100.0f);
		}
		StartLHandRumble("GrabL", data.giant, 0.5, 0.10);
	}

	void GTSGrab_Catch_Actor(AnimationEventData& data) {
		auto giant = &data.giant;
		giant->SetGraphVariableInt("GTS_GrabbedTiny", 1);
		auto grabbedActor = Grab::GetHeldActor(&data.giant);
		if (grabbedActor) {
			Grab::AttachActorTask(giant, grabbedActor);
			DisableCollisions(grabbedActor, &data.giant); // Just to be sure
			if (!IsTeammate(grabbedActor)) {
				Attacked(grabbedActor, giant);
			}
		}
		Rumbling::Once("GrabCatch", giant, 2.0, 0.15);
	}

	void GTSGrab_Catch_End(AnimationEventData& data) {
		ManageCamera(&data.giant, false, CameraTracking::Grab_Left);
		StopLHandRumble("GrabL", data.giant);
	}

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	/////////////////////////R E L E A S E
	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	void GTSGrab_Release_FreeActor(AnimationEventData& data) {
		auto giant = &data.giant;
		
		giant->SetGraphVariableInt("GTS_GrabbedTiny", 0);
		giant->SetGraphVariableInt("GTS_Storing_Tiny", 0);
		giant->SetGraphVariableInt("GTS_Grab_State", 0);
		auto grabbedActor = Grab::GetHeldActor(giant);
		ManageCamera(&data.giant, false, CameraTracking::Grab_Left);
		AnimationManager::StartAnim("TinyDied", giant);
		//BlockFirstPerson(giant, false);
		if (grabbedActor) {
			SetBetweenBreasts(grabbedActor, false);
			PushActorAway(giant, grabbedActor, 1.0);
			EnableCollisions(grabbedActor);
			SetBeingHeld(grabbedActor, false);
		}
		Grab::DetachActorTask(giant);
		Grab::Release(giant);
	}

	void GTSBEH_GrabExit(AnimationEventData& data) {
		auto giant = &data.giant;
		auto grabbedActor = Grab::GetHeldActor(giant);
		if (grabbedActor) {
			EnableCollisions(grabbedActor);
			SetBetweenBreasts(grabbedActor, false);
		}
		

		giant->SetGraphVariableInt("GTS_GrabbedTiny", 0);
		giant->SetGraphVariableInt("GTS_Storing_Tiny", 0);
		giant->SetGraphVariableInt("GTS_Grab_State", 0);
		AnimationManager::StartAnim("TinyDied", giant);
		DrainStamina(giant, "GrabAttack", "DestructionBasics", false, 0.75);
		DrainStamina(giant, "GrabThrow", "DestructionBasics", false, 1.25);
		ManageCamera(&data.giant, false, CameraTracking::Grab_Left);
		Grab::DetachActorTask(giant);
		Grab::Release(giant);
	}

	void GTSBEH_AbortGrab(AnimationEventData& data) {
		auto giant = &data.giant;
		auto grabbedActor = Grab::GetHeldActor(giant);
		if (grabbedActor) {
			EnableCollisions(grabbedActor);
			SetBeingHeld(grabbedActor, false);
			SetBetweenBreasts(grabbedActor, false);
		}
		
		giant->SetGraphVariableInt("GTS_GrabbedTiny", 0);
		giant->SetGraphVariableInt("GTS_Storing_Tiny", 0);
		giant->SetGraphVariableInt("GTS_Grab_State", 0);

		AnimationManager::StartAnim("TinyDied", giant);
		DrainStamina(giant, "GrabAttack", "DestructionBasics", false, 0.75);
		DrainStamina(giant, "GrabThrow", "DestructionBasics", false, 1.25);
		ManageCamera(&data.giant, false, CameraTracking::Grab_Left);
		Grab::DetachActorTask(giant);
		Grab::Release(giant);
	}

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	/////////////////////////B R E A S T S
	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	void GTSGrab_Breast_MoveStart(AnimationEventData& data) {
		ManageCamera(&data.giant, true, CameraTracking::Grab_Left);
	}

	void GTSGrab_Breast_PutActor(AnimationEventData& data) { // Places actor between breasts
		auto giant = &data.giant;
		
		Runtime::PlaySoundAtNode("BreastImpact", giant, 1.0, 0.0, "NPC L Hand [LHnd]");
		giant->SetGraphVariableInt("GTS_Storing_Tiny", 1);
		giant->SetGraphVariableInt("GTS_GrabbedTiny", 0);
		auto otherActor = Grab::GetHeldActor(giant);
		if (otherActor) {
			SetBetweenBreasts(otherActor, true);
			Task_RotateActorToBreastX(giant, otherActor);
			otherActor->SetGraphVariableBool("GTSBEH_T_InStorage", true);
			if (IsHostile(giant, otherActor)) {
				AnimationManager::StartAnim("Breasts_Idle_Unwilling", otherActor);
			} else {
				AnimationManager::StartAnim("Breasts_Idle_Willing", otherActor);
			}
		}
	}

	void GTSGrab_Breast_TakeActor(AnimationEventData& data) { // Removes Actor
		auto giant = &data.giant;
		giant->SetGraphVariableInt("GTS_Storing_Tiny", 0);
		giant->SetGraphVariableInt("GTS_GrabbedTiny", 1);
		auto otherActor = Grab::GetHeldActor(giant);
		if (otherActor) {
			SetBetweenBreasts(otherActor, false);
			otherActor->SetGraphVariableBool("GTSBEH_T_InStorage", false);
			//BlockFirstPerson(giant, true);
			AnimationManager::StartAnim("Breasts_FreeOther", otherActor);
		}
	}

	void GTSGrab_Breast_MoveEnd(AnimationEventData& data) {
		ManageCamera(&data.giant, false, CameraTracking::Grab_Left);
	}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////// T R I G G E R S
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	void GrabOtherEvent(const InputEventData& data) { // Grab other actor
		auto player = PlayerCharacter::GetSingleton();
		auto grabbedActor = Grab::GetHeldActor(player);
		if (grabbedActor) { //If we have actor, don't pick anyone up.
			return;
		}
		if (!CanPerformAnimation(player, 2)) {
			return;
		}
		if (IsGtsBusy(player) || IsEquipBusy(player) || IsTransitioning(player)) {
			return; // Disallow Grabbing if Behavior is busy doing other stuff.
		}
		auto& Grabbing = GrabAnimationController::GetSingleton();

		std::vector<Actor*> preys = Grabbing.GetGrabTargetsInFront(player, 1.0);
		for (auto prey: preys) {
			Grabbing.StartGrab(player, prey);
		}
	}

	void GrabOtherEvent_Follower(const InputEventData& data) { // Force Follower to grab player
		Actor* player = PlayerCharacter::GetSingleton();
		ForceFollowerAnimation(player, FollowerAnimType::Grab);
	}

	void GrabAttackEvent(const InputEventData& data) { // Attack everyone in your hand
		Actor* player = GetPlayerOrControlled();

		if (IsGtsBusy(player) && !IsUsingThighAnimations(player)) {
			return;
		}
		if (!IsStomping(player) && !IsTransitioning(player)) {
			auto grabbedActor = Grab::GetHeldActor(player);
			if (!grabbedActor) {
				return;
			}
			float WasteStamina = 20.0;
			if (Runtime::HasPerk(player, "DestructionBasics")) {
				WasteStamina *= 0.65;
			}
			if (GetAV(player, ActorValue::kStamina) > WasteStamina) {
				AnimationManager::StartAnim("GrabDamageAttack", player);
			} else {
				TiredSound(player, "You're too tired to perform hand attack");
			}
		}
	}

	void GrabVoreEvent(const InputEventData& data) { // Eat everyone in hand
		Actor* player = GetPlayerOrControlled();

		if (!CanPerformAnimation(player, 3)) {
			return;
		}
		if (IsGtsBusy(player) && !IsUsingThighAnimations(player)) {
			return;
		}
		if (!IsTransitioning(player)) {
			auto grabbedActor = Grab::GetHeldActor(player);
			if (!grabbedActor) {
				return;
			}
			if (IsInsect(grabbedActor, true) || IsBlacklisted(grabbedActor) || IsUndead(grabbedActor, true)) {
				return; // Same rules as with Vore
			}
			AnimationManager::StartAnim("GrabEatSomeone", player);
		}
	}

	void GrabThrowEvent(const InputEventData& data) { // Throw everyone away
		Actor* player = GetPlayerOrControlled();

		if (IsGtsBusy(player) && !IsUsingThighAnimations(player)) {
			return;
		}
		if (!IsTransitioning(player)) { // Only allow outside of GtsBusy and when not transitioning
			auto grabbedActor = Grab::GetHeldActor(player);
			if (!grabbedActor) {
				return;
			}
			float WasteStamina = 40.0;
			if (Runtime::HasPerk(player, "DestructionBasics")) {
				WasteStamina *= 0.65;
			}
			if (GetAV(player, ActorValue::kStamina) > WasteStamina) {
				AnimationManager::StartAnim("GrabThrowSomeone", player);
			} else {
				TiredSound(player, "You're too tired to throw that actor");
			}
		}
	}

	void GrabReleaseEvent(const InputEventData& data) {
		Actor* player = GetPlayerOrControlled();

		auto grabbedActor = Grab::GetHeldActor(player);
		if (!grabbedActor) {
			return;
		}
		if (IsGtsBusy(player) && !IsUsingThighAnimations(player) || IsTransitioning(player)) {
			return;
		}
		Utils_UpdateHighHeelBlend(player, false);
		AnimationManager::StartAnim("GrabReleasePunies", player);
	}

	void BreastsPutEvent(const InputEventData& data) {
		Actor* player = GetPlayerOrControlled();

		auto grabbedActor = Grab::GetHeldActor(player);
		if (!grabbedActor || IsTransitioning(player)) {
			return;
		}
		AnimationManager::StartAnim("Breasts_Put", player);
	}
	void BreastsRemoveEvent(const InputEventData& data) {
		Actor* player = GetPlayerOrControlled();

		auto grabbedActor = Grab::GetHeldActor(player);
		if (!grabbedActor || IsTransitioning(player)) {
			return;
		}
		AnimationManager::StartAnim("Breasts_Pull", player);
	}
}





namespace Gts {

	void Utils_CrushTask(Actor* giant, Actor* grabbedActor, float bonus, bool do_sound) {
        
        auto tinyref = grabbedActor->CreateRefHandle();
        auto giantref = giant->CreateRefHandle();

        std::string taskname = std::format("GrabCrush_{}", grabbedActor->formID);

        TaskManager::RunOnce(taskname, [=](auto& update) {
            if (!tinyref) {
                return;
            } 
            if (!giantref) {
                return;
            }
            auto tiny = tinyref.get().get();
            auto giantess = giantref.get().get();

            if (GetAV(tiny, ActorValue::kHealth) <= 1.0 || tiny->IsDead()) {

                ModSizeExperience_Crush(giant, tiny, false);

                CrushManager::Crush(giantess, tiny);
                
                SetBeingHeld(tiny, false);

                Runtime::PlaySoundAtNode("DefaultCrush", giantess, 1.0, 1.0, "NPC L Hand [LHnd]");

				if (do_sound) {
					if (!LessGore()) {
						Runtime::PlaySoundAtNode("CrunchImpactSound", giantess, 1.0, 1.0, "NPC L Hand [LHnd]");
						Runtime::PlaySoundAtNode("CrunchImpactSound", giantess, 1.0, 1.0, "NPC L Hand [LHnd]");
					} else {
						Runtime::PlaySoundAtNode("SoftHandAttack", giantess, 1.0, 1.0, "NPC L Hand [LHnd]");
					}
				}

                AdjustSizeReserve(giantess, get_visual_scale(tiny)/10);
                SpawnHurtParticles(giantess, tiny, 3.0, 1.6);
                SpawnHurtParticles(giantess, tiny, 3.0, 1.6);
                
                SetBetweenBreasts(tiny, false);
                StartCombat(tiny, giantess);
                
                AdvanceQuestProgression(giantess, tiny, QuestStage::HandCrush, 1.0, false);
                
                PrintDeathSource(giantess, tiny, DamageSource::HandCrushed);
            } else {
				if (do_sound) {
					if (!LessGore()) {
						Runtime::PlaySoundAtNode("CrunchImpactSound", giantess, 1.0, 1.0, "NPC L Hand [LHnd]");
						SpawnHurtParticles(giantess, tiny, 1.0, 1.0);
					} else {
						Runtime::PlaySoundAtNode("SoftHandAttack", giantess, 1.0, 1.0, "NPC L Hand [LHnd]");
					}
				}
                StaggerActor(giantess, tiny, 0.75f);
            }
        });
    }
    

	Grab& Grab::GetSingleton() noexcept {
		static Grab instance;
		return instance;
	}

	std::string Grab::DebugName() {
		return "Grab";
	}

	void Grab::DamageActorInHand(Actor* giant, float Damage) {
        Actor* grabbed = Grab::GetHeldActor(giant);
		auto& sizemanager = SizeManager::GetSingleton();

        if (grabbed) {
            Attacked(grabbed, giant); // force combat

            float bonus = 1.0;

			float tiny_scale = get_visual_scale(grabbed) * GetSizeFromBoundingBox(grabbed);
			float gts_scale = get_visual_scale(giant) * GetSizeFromBoundingBox(giant);

			float sizeDiff = gts_scale/tiny_scale;
			float power = std::clamp(sizemanager.GetSizeAttribute(giant, SizeAttribute::Normal), 1.0f, 999999.0f);
			float additionaldamage = 1.0 + sizemanager.GetSizeVulnerability(grabbed);
			float damage = (Damage * sizeDiff) * power * additionaldamage * additionaldamage;
			float experience = std::clamp(damage/1600, 0.0f, 0.06f);
			if (HasSMT(giant)) {
				bonus = 1.65;
			}

            if (CanDoDamage(giant, grabbed, false)) {
                if (Runtime::HasPerkTeam(giant, "GrowingPressure")) {
                    auto& sizemanager = SizeManager::GetSingleton();
                    sizemanager.ModSizeVulnerability(grabbed, damage * 0.0010);
                }

                TinyCalamity_ShrinkActor(giant, grabbed, damage * 0.10 * GetDamageSetting());

                SizeHitEffects::GetSingleton().BreakBones(giant, grabbed, 0.15, 6);
                InflictSizeDamage(giant, grabbed, damage);
            }
			
			Rumbling::Once("GrabAttack", giant, Rumble_Grab_Hand_Attack * bonus, 0.05, "NPC L Hand [LHnd]", 0.0);

			ModSizeExperience(giant, experience);
			AddSMTDuration(giant, 1.0);

            Utils_CrushTask(giant, grabbed, bonus, false);
        }
    }

	void Grab::DetachActorTask(Actor* giant) {
		std::string name = std::format("GrabAttach_{}", giant->formID);
		giant->SetGraphVariableInt("GTS_GrabbedTiny", 0); // Tell behaviors 'we have nothing in our hands'. A must.
		giant->SetGraphVariableInt("GTS_Grab_State", 0);
		giant->SetGraphVariableInt("GTS_Storing_Tiny", 0);
		TaskManager::Cancel(name);
		Grab::Release(giant);
	}

	void Grab::AttachActorTask(Actor* giant, Actor* tiny) {
		if (!giant) {
			return;
		}
		if (!tiny) {
			return;
		}
		std::string name = std::format("GrabAttach_{}", giant->formID);
		ActorHandle gianthandle = giant->CreateRefHandle();
		ActorHandle tinyhandle = tiny->CreateRefHandle();
		TaskManager::Run(name, [=](auto& progressData) {
			if (!gianthandle) {
				return false;
			}
			if (!tinyhandle) {
				return false;
			}
			auto giantref = gianthandle.get().get();
			auto tinyref = tinyhandle.get().get();

			if (!tinyref) {
				return false; // end task in that case
			}

			// Exit on death
			float scale_gts = get_target_scale(giant) * GetSizeFromBoundingBox(giant);
			float scale_tiny = get_target_scale(tiny) * GetSizeFromBoundingBox(tiny);
			float sizedifference = scale_gts/scale_tiny;

			ForceRagdoll(tinyref, false); 

			ShutUp(tinyref);

			bool Attacking = false;
			giantref->GetGraphVariableBool("GTS_IsGrabAttacking", Attacking);
			if (!Attacking || IsBeingEaten(tinyref)) {
				if (giantref->IsDead() || tinyref->IsDead() || GetAV(tinyref, ActorValue::kHealth) <= 0.0 || sizedifference < Action_Grab || GetAV(giantref, ActorValue::kStamina) < 2.0) {
					PushActorAway(giantref, tinyref, 1.0);
					tinyref->SetGraphVariableBool("GTSBEH_T_InStorage", false);
					SetBetweenBreasts(tinyref, false);
					SetBeingHeld(tinyref, false);
					giantref->SetGraphVariableInt("GTS_GrabbedTiny", 0); // Tell behaviors 'we have nothing in our hands'. A must.
					giantref->SetGraphVariableInt("GTS_Grab_State", 0);
					giantref->SetGraphVariableInt("GTS_Storing_Tiny", 0);
					DrainStamina(giant, "GrabAttack", "DestructionBasics", false, 0.75);
					AnimationManager::StartAnim("GrabAbort", giantref); // Abort Grab animation
					AnimationManager::StartAnim("TinyDied", giantref);
					ManageCamera(giantref, false, CameraTracking::Grab_Left); // Disable any camera edits
					Grab::DetachActorTask(giantref);
					Grab::Release(giantref);
					return false;
				}
			}

			if (IsBeingEaten(tinyref)) {
				if (!AttachToObjectA(gianthandle, tinyhandle)) {
					// Unable to attach
					return false;
				}
			} else if (IsBetweenBreasts(tinyref)) {
				bool hostile = IsHostile(giantref, tinyref);
				float restore = 0.04 * TimeScale();
				if (!hostile) {
					tinyref->AsActorValueOwner()->RestoreActorValue(ACTOR_VALUE_MODIFIER::kDamage, ActorValue::kHealth, restore);
					tinyref->AsActorValueOwner()->RestoreActorValue(ACTOR_VALUE_MODIFIER::kDamage, ActorValue::kStamina, restore);
				}
				if (hostile) {
					DamageAV(tinyref, ActorValue::kStamina, restore * 2);
				}
				if (!AttachToCleavage(gianthandle, tinyhandle)) {
					// Unable to attach
					Grab::Release(giantref);
					return false;
				}
			} else if (AttachToHand(gianthandle, tinyhandle)) {
				GrabStaminaDrain(giantref, tinyref, sizedifference);
				return true;
			} else {
				if (!AttachToHand(gianthandle, tinyhandle)) {
					// Unable to attach
					return false;
				}
			}

			// All good try another frame
			return true;
		});
		TaskManager::ChangeUpdate(name, UpdateKind::Havok);
	}


	void Grab::GrabActor(Actor* giant, TESObjectREFR* tiny, float strength) {
		Grab::GetSingleton().data.try_emplace(giant, tiny, strength);
	}
	void Grab::GrabActor(Actor* giant, TESObjectREFR* tiny) {
		// Default strength 1.0: normal grab for actor of their size
		//
		Grab::GrabActor(giant, tiny, 1.0);
	}

	void Grab::Reset() {
		this->data.clear();
	}

	void Grab::ResetActor(Actor* actor) {
		this->data.erase(actor);
	}

	void Grab::Release(Actor* giant) {
		Grab::GetSingleton().data.erase(giant);
	}

	TESObjectREFR* Grab::GetHeldObj(Actor* giant) {
		try {
			auto& me = Grab::GetSingleton();
			return me.data.at(giant).tiny;
		} catch (std::out_of_range e) {
			return nullptr;
		}

	}
	Actor* Grab::GetHeldActor(Actor* giant) {
		auto obj = Grab::GetHeldObj(giant);
		Actor* actor = skyrim_cast<Actor*>(obj);
		if (actor) {
			return actor;
		} else {
			return nullptr;
		}
	}

	void Grab::RegisterTriggers() {
		AnimationManager::RegisterTrigger("GrabSomeone", "Grabbing", "GTSBEH_GrabStart");
		AnimationManager::RegisterTrigger("GrabEatSomeone", "Grabbing", "GTSBEH_GrabVore");
		AnimationManager::RegisterTrigger("GrabDamageAttack", "Grabbing", "GTSBEH_GrabAttack");
		AnimationManager::RegisterTrigger("GrabThrowSomeone", "Grabbing", "GTSBEH_GrabThrow");
		AnimationManager::RegisterTrigger("GrabReleasePunies", "Grabbing", "GTSBEH_GrabRelease");
		AnimationManager::RegisterTrigger("GrabExit", "Grabbing", "GTSBEH_GrabExit");
		AnimationManager::RegisterTrigger("GrabAbort", "Grabbing", "GTSBEH_AbortGrab");
		AnimationManager::RegisterTrigger("TinyDied", "Grabbing", "GTSBEH_TinyDied");
		AnimationManager::RegisterTrigger("Breasts_Put", "Grabbing", "GTSBEH_BreastsAdd");
		AnimationManager::RegisterTrigger("Breasts_Pull", "Grabbing", "GTSBEH_BreastsRemove");
		AnimationManager::RegisterTrigger("Breasts_Idle_Unwilling", "Grabbing", "GTSBEH_T_Storage_Enemy");
		AnimationManager::RegisterTrigger("Breasts_Idle_Willing", "Grabbing", "GTSBEH_T_Storage_Ally");
		AnimationManager::RegisterTrigger("Breasts_FreeOther", "Grabbing", "GTSBEH_T_Remove");

	}

	void StartRHandRumble(std::string_view tag, Actor& actor, float power, float halflife) {
		for (auto& node_name: RHAND_RUMBLE_NODES) {
			std::string rumbleName = std::format("{}{}", tag, node_name);
			Rumbling::Start(rumbleName, &actor, power,  halflife, node_name);
		}
	}

	void StartLHandRumble(std::string_view tag, Actor& actor, float power, float halflife) {
		for (auto& node_name: LHAND_RUMBLE_NODES) {
			std::string rumbleName = std::format("{}{}", tag, node_name);
			Rumbling::Start(rumbleName, &actor, power,  halflife, node_name);
		}
	}

	void StopRHandRumble(std::string_view tag, Actor& actor) {
		for (auto& node_name: RHAND_RUMBLE_NODES) {
			std::string rumbleName = std::format("{}{}", tag, node_name);
			Rumbling::Stop(rumbleName, &actor);
		}
	}
	void StopLHandRumble(std::string_view tag, Actor& actor) {
		for (auto& node_name: RHAND_RUMBLE_NODES) {
			std::string rumbleName = std::format("{}{}", tag, node_name);
			Rumbling::Stop(rumbleName, &actor);
		}
	}

	void Grab::RegisterEvents() {
		InputManager::RegisterInputEvent("GrabPlayer", GrabOtherEvent_Follower);
		InputManager::RegisterInputEvent("GrabOther", GrabOtherEvent);
		InputManager::RegisterInputEvent("GrabAttack", GrabAttackEvent);
		InputManager::RegisterInputEvent("GrabVore", GrabVoreEvent);
		InputManager::RegisterInputEvent("GrabThrow", GrabThrowEvent);
		InputManager::RegisterInputEvent("GrabRelease", GrabReleaseEvent);
		InputManager::RegisterInputEvent("BreastsPut", BreastsPutEvent);
		InputManager::RegisterInputEvent("BreastsRemove", BreastsRemoveEvent);

		AnimationManager::RegisterEvent("GTSGrab_Catch_Start", "Grabbing", GTSGrab_Catch_Start);
		AnimationManager::RegisterEvent("GTSGrab_Catch_Actor", "Grabbing", GTSGrab_Catch_Actor);
		AnimationManager::RegisterEvent("GTSGrab_Catch_End", "Grabbing", GTSGrab_Catch_End);

		AnimationManager::RegisterEvent("GTSGrab_Breast_MoveStart", "Grabbing", GTSGrab_Breast_MoveStart);
		AnimationManager::RegisterEvent("GTSGrab_Breast_PutActor", "Grabbing", GTSGrab_Breast_PutActor);
		AnimationManager::RegisterEvent("GTSGrab_Breast_TakeActor", "Grabbing", GTSGrab_Breast_TakeActor);
		AnimationManager::RegisterEvent("GTSGrab_Breast_MoveEnd", "Grabbing", GTSGrab_Breast_MoveEnd);

		AnimationManager::RegisterEvent("GTSGrab_Release_FreeActor", "Grabbing", GTSGrab_Release_FreeActor);

		AnimationManager::RegisterEvent("GTSBEH_GrabExit", "Grabbing", GTSBEH_GrabExit);
		AnimationManager::RegisterEvent("GTSBEH_AbortGrab", "Grabbing", GTSBEH_AbortGrab);
	}

	GrabData::GrabData(TESObjectREFR* tiny, float strength) : tiny(tiny), strength(strength) {
	}
}


//Beh's:
/*
        GTSBEH_GrabStart
        GTSBEH_GrabVore
        GTSBEH_GrabAttack
        GTSBEH_GrabThrow
        GTSBEH_GrabRelease

        GTSBeh_GrabExit
        GTSBEH_AbortGrab (Similar to GTSBEH_Exit but Grab only)
 */
