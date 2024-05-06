#include "managers/animation/Utils/AnimationUtils.hpp"
#include "managers/animation/AnimationManager.hpp"
#include "managers/animation/Utils/CrawlUtils.hpp"
#include "managers/emotions/EmotionManager.hpp"
#include "managers/animation/Vore_Crawl.hpp"
#include "managers/GtsSizeManager.hpp"
#include "managers/ai/aifunctions.hpp"
#include "managers/CrushManager.hpp"
#include "utils/papyrusUtils.hpp"
#include "managers/explosion.hpp"
#include "managers/audio/footstep.hpp"
#include "utils/actorUtils.hpp"
#include "data/persistent.hpp"
#include "managers/Rumble.hpp"
#include "managers/tremor.hpp"
#include "ActionSettings.hpp"
#include "data/transient.hpp"
#include "managers/vore.hpp"
#include "data/runtime.hpp"
#include "scale/scale.hpp"
#include "node.hpp"

using namespace std;
using namespace SKSE;
using namespace RE;
using namespace Gts;

namespace {
	const std::string_view RNode = "NPC R Foot [Rft ]";
	const std::string_view LNode = "NPC L Foot [Lft ]";

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

	const std::vector<std::string_view> BODY_RUMBLE_NODES = { // used for body rumble
		"NPC COM [COM ]",
		"NPC L Foot [Lft ]",
		"NPC R Foot [Rft ]",
		"NPC L Toe0 [LToe]",
		"NPC R Toe0 [RToe]",
		"NPC L Calf [LClf]",
		"NPC R Calf [RClf]",
		"NPC L PreRearCalf",
		"NPC R PreRearCalf",
		"NPC L FrontThigh",
		"NPC R FrontThigh",
		"NPC R RearCalf [RrClf]",
		"NPC L RearCalf [RrClf]",
	};

	void GTSBeh_CrawlVoring(AnimationEventData& data) {
		auto giant = &data.giant;
		auto& VoreData = Vore::GetSingleton().GetVoreData(giant);
		VoreData.AllowToBeVored(false);
		for (auto& tiny: VoreData.GetVories()) {
			AllowToBeCrushed(tiny, false);
			DisableCollisions(tiny, giant);
			SetBeingHeld(tiny, true);
		}
	}

	void GTSCrawlVore_SmileOn(AnimationEventData& data) {
		AdjustFacialExpression(&data.giant, 2, 1.0, "expression");
		AdjustFacialExpression(&data.giant, 3, 0.8, "phenome");
	}

	void GTSCrawlVore_Grab(AnimationEventData& data) {
		auto giant = &data.giant;
		auto& VoreData = Vore::GetSingleton().GetVoreData(giant);
		for (auto& tiny: VoreData.GetVories()) {
			if (!Vore_ShouldAttachToRHand(giant, tiny)) {
				VoreData.GrabAll();
			}
			tiny->NotifyAnimationGraph("JumpFall");
			Attacked(tiny, giant);
		}
		if (IsTransferingTiny(giant)) {
			ManageCamera(giant, true, CameraTracking::ObjectA);
		} else {
			ManageCamera(giant, true, CameraTracking::Hand_Right);
		}
	}

	void GTSCrawl_ButtImpact(AnimationEventData& data) {
		auto giant = &data.giant;

		float perk = GetPerkBonus_Basics(&data.giant);
		float dust = 1.0;
		float smt = 1.0;

		if (HasSMT(giant)) {
			dust = 1.25;
			smt = 2.0;
		}

		auto ThighL = find_node(giant, "NPC L Thigh [LThg]");
		auto ThighR = find_node(giant, "NPC R Thigh [RThg]");
		auto ButtR = find_node(giant, "NPC R Butt");
		auto ButtL = find_node(giant, "NPC L Butt");
		if (ButtR && ButtL) {
			if (ThighL && ThighR) {
				
				ApplyThighDamage(giant, true, false, Radius_ThighCrush_ButtCrush_Drop, Damage_ButtCrush_LegDrop * perk, 0.35, 1.0, 14, DamageSource::ThighCrushed);
				ApplyThighDamage(giant, false, false, Radius_ThighCrush_ButtCrush_Drop, Damage_ButtCrush_LegDrop * perk, 0.35, 1.0, 14, DamageSource::ThighCrushed);

				DoDamageAtPoint(giant, Radius_Crawl_Vore_ButtImpact, Damage_Crawl_Vore_Butt_Impact * perk, ThighL, 10, 0.70, 0.95, DamageSource::Booty);
				DoDamageAtPoint(giant, Radius_Crawl_Vore_ButtImpact, Damage_Crawl_Vore_Butt_Impact * perk, ThighR, 10, 0.70, 0.95, DamageSource::Booty);
				DoDustExplosion(giant, 1.8 * dust, FootEvent::Right, "NPC R Butt");
				DoDustExplosion(giant, 1.8 * dust, FootEvent::Left, "NPC L Butt");
				DoFootstepSound(giant, 1.2, FootEvent::Right, RNode);
				DoFootstepSound(giant, 1.2, FootEvent::Left, LNode);
				DoLaunch(&data.giant, 0.95, 4.2, FootEvent::Butt);

				float shake_power = Rumble_Crawl_KneeDrop/2 * smt;

				Rumbling::Once("Butt_L", &data.giant, shake_power, 0.10, "NPC R Butt", 0.0);
				Rumbling::Once("Butt_R", &data.giant, shake_power, 0.10, "NPC L Butt",  0.0);
			}
		}
	}

	void GTSCrawlVore_OpenMouth(AnimationEventData& data) {
		auto giant = &data.giant;
		AdjustFacialExpression(giant, 0, 1.0, "phenome"); // Start opening mouth
		AdjustFacialExpression(giant, 1, 0.5, "phenome"); // Open it wider
		AdjustFacialExpression(giant, 0, 0.80, "modifier"); // blink L
		AdjustFacialExpression(giant, 1, 0.80, "modifier"); // blink R
	}

	void GTSCrawlVore_CloseMouth(AnimationEventData& data) {
		auto giant = &data.giant;
		AdjustFacialExpression(giant, 0, 0.0, "phenome"); // Start opening mouth
		AdjustFacialExpression(giant, 1, 0.0, "phenome"); // Open it wider
		AdjustFacialExpression(giant, 0, 0.0, "modifier"); // blink L
		AdjustFacialExpression(giant, 1, 0.0, "modifier"); // blink R
	}
	void GTSCrawlVore_Swallow(AnimationEventData& data) {
		auto giant = &data.giant;
		auto& VoreData = Vore::GetSingleton().GetVoreData(giant);

		for (auto& tiny: VoreData.GetVories()) {
			AllowToBeCrushed(tiny, true);
			if (tiny->formID == 0x14) {
				PlayerCamera::GetSingleton()->cameraTarget = giant->CreateRefHandle();
			}
			if (AllowDevourment()) {
				CallDevourment(&data.giant, tiny);
				SetBeingHeld(tiny, false);
				VoreData.AllowToBeVored(true);
			} else {
				VoreData.Swallow();
				tiny->SetAlpha(0.0);
				Runtime::PlaySoundAtNode("VoreSwallow", giant, 1.0, 1.0, "NPC Head [Head]"); // Play sound
			}
		}
	}

	void GTSCrawlVore_KillAll(AnimationEventData& data) {
		auto giant = &data.giant;
		auto& VoreData = Vore::GetSingleton().GetVoreData(giant);
		for (auto& tiny: VoreData.GetVories()) {
			if (tiny) {
				AllowToBeCrushed(tiny, true);
				EnableCollisions(tiny);
			}
		}
		VoreData.AllowToBeVored(true);
		VoreData.KillAll();
		VoreData.ReleaseAll();

		ManageCamera(giant, false, CameraTracking::ObjectA);
		ManageCamera(giant, false, CameraTracking::Hand_Right);
	}

	void GTSCrawlVore_SmileOff(AnimationEventData& data) {
		AdjustFacialExpression(&data.giant, 2, 0.0, "expression");
		AdjustFacialExpression(&data.giant, 3, 0.0, "phenome");
	}

	void GTSBEH_CrawlVoreExit(AnimationEventData& data) {} // unused
}


namespace Gts
{
	void Animation_VoreCrawl::RegisterEvents() { 
		AnimationManager::RegisterEvent("GTSBeh_CrawlVoring", "CrawlVore", GTSBeh_CrawlVoring);
		AnimationManager::RegisterEvent("GTSCrawlVore_SmileOn", "CrawlVore", GTSCrawlVore_SmileOn);
		AnimationManager::RegisterEvent("GTSCrawlVore_Grab", "CrawlVore", GTSCrawlVore_Grab);
		AnimationManager::RegisterEvent("GTSCrawl_ButtImpact", "CrawlVore", GTSCrawl_ButtImpact);
		AnimationManager::RegisterEvent("GTSBEH_CrawlVoreExit", "CrawlVore", GTSBEH_CrawlVoreExit);
		AnimationManager::RegisterEvent("GTSCrawlVore_OpenMouth", "CrawlVore", GTSCrawlVore_OpenMouth);
		AnimationManager::RegisterEvent("GTSCrawlVore_CloseMouth", "CrawlVore", GTSCrawlVore_CloseMouth);
		AnimationManager::RegisterEvent("GTSCrawlVore_Swallow", "CrawlVore", GTSCrawlVore_Swallow);
		AnimationManager::RegisterEvent("GTSCrawlVore_KillAll", "CrawlVore", GTSCrawlVore_KillAll);
		AnimationManager::RegisterEvent("GTSCrawlVore_SmileOff", "CrawlVore", GTSCrawlVore_SmileOff);
	}
}
