#include "managers/animation/Utils/CooldownManager.hpp"
#include "managers/animation/Utils/AnimationUtils.hpp"
#include "managers/animation/AnimationManager.hpp"
#include "managers/damage/CollisionDamage.hpp"
#include "managers/GtsSizeManager.hpp"
#include "managers/InputManager.hpp"
#include "managers/CrushManager.hpp"
#include "magic/effects/common.hpp"
#include "utils/InputFunctions.hpp"
#include "managers/Attributes.hpp"
#include "managers/highheel.hpp"
#include "utils/actorUtils.hpp"
#include "data/persistent.hpp"
#include "managers/Rumble.hpp"
#include "ActionSettings.hpp"
#include "data/transient.hpp"
#include "data/runtime.hpp"
#include "data/plugin.hpp"
#include "scale/scale.hpp"
#include "data/time.hpp"
#include "utils/av.hpp"
#include "timer.hpp"

using namespace RE;
using namespace Gts;


namespace {
	void ReportScaleIntoConsole(Actor* actor, bool enemy) {
		float hh = HighHeelManager::GetBaseHHOffset(actor)[2]/100;
		float gigantism = Ench_Aspect_GetPower(actor) * 100;
		float naturalscale = get_natural_scale(actor, true);
		float scale = get_visual_scale(actor);
		float maxscale = get_max_scale(actor) * naturalscale;

		Actor* player = PlayerCharacter::GetSingleton();

		float BB = GetSizeFromBoundingBox(actor);
		if (enemy) {
			Cprint("{} Bounding Box To Size: {:.2f}, GameScale: {:.2f}", actor->GetDisplayFullName(), BB, game_getactorscale(actor));
			Cprint("{} Size Difference With the Player: {:.2f}", actor->GetDisplayFullName(), GetSizeDifference(player, actor, SizeType::VisualScale, false, true));
		} else {
			Cprint("{} Height: {:.2f} m / {:.2f} ft; Weight: {:.2f} kg / {:.2f} lb;", actor->GetDisplayFullName(), GetActorHeight(actor, true), GetActorHeight(actor, false), GetActorWeight(actor, true), GetActorWeight(actor, false));
		}

		if (maxscale > 250.0 * naturalscale) {
			Cprint("{} Scale: {:.2f}  (Natural Scale: {:.2f}; Size Limit: Infinite; Aspect Of Giantess: {:.1f}%)", actor->GetDisplayFullName(), scale, naturalscale, gigantism);
		} else {
			Cprint("{} Scale: {:.2f}  (Natural Scale: {:.2f}; Size Limit: {:.2f}; Aspect Of Giantess: {:.1f}%)", actor->GetDisplayFullName(), scale, naturalscale, maxscale, gigantism);
		}
		if (hh > 0.0) { // if HH is > 0, print HH info
			Cprint("{} High Heels: {:.2f} (+{:.2f} cm / +{:.2f} ft)", actor->GetDisplayFullName(), hh, hh, hh*3.28);
		}
	}
	void ReportScale(bool enemy) {
		for (auto actor: find_actors()) {
			if (actor->formID != 0x14) {
				if (enemy && !IsTeammate(actor)) {
					ReportScaleIntoConsole(actor, enemy);
				} else if (IsTeammate(actor)) {
					ReportScaleIntoConsole(actor, false);
				}
			} else {
				if (!enemy) {
					ReportScaleIntoConsole(actor, false);
				}
			}
		}
	}

	void regenerate_health(Actor* giant, float value) {
		if (Runtime::HasPerk(giant, "SizeReserveAug2")) {
			float maxhp = GetMaxAV(giant, ActorValue::kHealth);
			float regenerate = maxhp * 0.25 * value; // 25% of health

			giant->AsActorValueOwner()->RestoreActorValue(ACTOR_VALUE_MODIFIER::kDamage, ActorValue::kHealth, regenerate * TimeScale());
		}
	}

	void TotalControlGrowEvent(const InputEventData& data) {
		auto player = PlayerCharacter::GetSingleton();
		if (Runtime::HasPerk(player, "GrowthDesirePerkAug")) {
			float scale = get_visual_scale(player);
			float stamina = std::clamp(GetStaminaPercentage(player), 0.05f, 1.0f);

			float perk = Perk_GetCostReduction(player);

			DamageAV(player, ActorValue::kStamina, 0.15 * perk * (scale * 0.5 + 0.5) * stamina * TimeScale());
			Grow(player, 0.0010 * stamina, 0.0);
			float Volume = std::clamp(get_visual_scale(player)/16.0f, 0.20f, 2.0f);
			Rumbling::Once("ColossalGrowth", player, 0.15, 0.05);
			static Timer timergrowth = Timer(2.00);
			if (timergrowth.ShouldRun()) {
				Runtime::PlaySoundAtNode("growthSound", player, Volume, 1.0, "NPC Pelvis [Pelv]");
			}
		}
	}
	void TotalControlShrinkEvent(const InputEventData& data) {
		auto player = PlayerCharacter::GetSingleton();
		if (Runtime::HasPerk(player, "GrowthDesirePerkAug")) {
			float scale = get_visual_scale(player);
			float stamina = std::clamp(GetStaminaPercentage(player), 0.05f, 1.0f);

			float perk = Perk_GetCostReduction(player);

			if (get_target_scale(player) > 0.12) {
				DamageAV(player, ActorValue::kStamina, 0.07 * perk * (scale * 0.5 + 0.5) * stamina * TimeScale());
				ShrinkActor(player, 0.0010 * stamina, 0.0);
			} else {
				set_target_scale(player, 0.12);
			}

			float Volume =std::clamp(get_visual_scale(player)*0.10f, 0.10f, 1.0f);
			Rumbling::Once("ColossalGrowth", player, 0.15, 0.05);
			static Timer timergrowth = Timer(2.00);
			if (timergrowth.ShouldRun()) {
				Runtime::PlaySound("shrinkSound", player, Volume, 1.0);
			}
		}
	}
	void TotalControlGrowOtherEvent(const InputEventData& data) {
		auto player = PlayerCharacter::GetSingleton();
		if (Runtime::HasPerk(player, "GrowthDesirePerkAug")) {
			for (auto actor: find_actors()) {
				if (!actor) {
					continue;
				}
				if (actor->formID != 0x14 && (IsTeammate(actor))) {

					float perk = Perk_GetCostReduction(player);

					float npcscale = get_visual_scale(actor);
					float magicka = std::clamp(GetMagikaPercentage(player), 0.05f, 1.0f);
					DamageAV(player, ActorValue::kMagicka, 0.15 * perk * (npcscale * 0.5 + 0.5) * magicka * TimeScale());
					Grow(actor, 0.0010 * magicka, 0.0);
					float Volume = std::clamp(0.20f, 2.0f, get_visual_scale(actor)/16.0f);
					Rumbling::Once("TotalControlOther", actor, 0.15, 0.05);
					static Timer timergrowth = Timer(2.00);
					if (timergrowth.ShouldRun()) {
						Runtime::PlaySoundAtNode("growthSound", actor, Volume, 1.0, "NPC Pelvis [Pelv]");
					}
				}
			}
		}
	}
	void TotalControlShrinkOtherEvent(const InputEventData& data) {
		auto player = PlayerCharacter::GetSingleton();
		if (Runtime::HasPerk(player, "GrowthDesirePerkAug")) {
			for (auto actor: find_actors()) {
				if (!actor) {
					continue;
				}
				if (actor->formID != 0x14 && (IsTeammate(actor))) {
					
					float perk = Perk_GetCostReduction(player);

					float npcscale = get_visual_scale(actor);
					float magicka = std::clamp(GetMagikaPercentage(player), 0.05f, 1.0f);
					DamageAV(player, ActorValue::kMagicka, 0.07 * perk * (npcscale * 0.5 + 0.5) * magicka * TimeScale());
					ShrinkActor(actor, 0.0010 * magicka, 0.0);
					float Volume = std::clamp(get_visual_scale(actor) * 0.10f, 0.10f, 1.0f);
					Rumbling::Once("TotalControlOther", actor, 0.15, 0.05);
					static Timer timergrowth = Timer(2.00);
					if (timergrowth.ShouldRun()) {
						Runtime::PlaySound("shrinkSound", actor, Volume, 1.0);
					}
				} 
			}
		}
	}

	void RapidGrowthEvent(const InputEventData& data) {
		auto player = PlayerCharacter::GetSingleton();
		if (!Runtime::HasPerk(player, "GrowthDesirePerkAug")) {
			return;
		}
		if (!IsGtsBusy(player) && !IsChangingSize(player)) {
			float target = get_target_scale(player);
			float max_scale = get_max_scale(player) * get_natural_scale(player);
			if (target >= max_scale) {
				TiredSound(player, "You can't grow any further");
				Rumbling::Once("CantGrow", player, 0.25, 0.05);
				return;
			}
			AnimationManager::StartAnim("TriggerGrowth", player);
		}
		
	}
	void RapidShrinkEvent(const InputEventData& data) {
		auto player = PlayerCharacter::GetSingleton();
		if (!Runtime::HasPerk(player, "GrowthDesirePerkAug")) {
			return;
		}
		if (!IsGtsBusy(player) && !IsChangingSize(player)) {
			float target = get_target_scale(player);
			if (target <= Minimum_Actor_Scale) {
				TiredSound(player, "You can't shrink any further");
				Rumbling::Once("CantGrow", player, 0.25, 0.05);
				return;
			}
			AnimationManager::StartAnim("TriggerShrink", player);
		}
	}

	void SizeReserveEvent(const InputEventData& data) {
		auto player = PlayerCharacter::GetSingleton();
		auto Cache = Persistent::GetSingleton().GetData(player);
		if (!Cache) {
			return;
		}
		if (Cache->SizeReserve > 0.0) {
			bool Attacking = false;
			player->GetGraphVariableBool("GTS_IsGrabAttacking", Attacking);

			if (!Attacking) {
				float duration = data.Duration();
				

				if (duration >= 1.2 && Runtime::HasPerk(player, "SizeReserve") && Cache->SizeReserve > 0) {
					float SizeCalculation = duration - 1.2;
					float gigantism = 1.0 + Ench_Aspect_GetPower(player);
					float Volume = std::clamp(get_visual_scale(player) * Cache->SizeReserve/10.0f, 0.10f, 2.0f);
					static Timer timergrowth = Timer(3.00);
					if (timergrowth.ShouldRunFrame()) {
						Runtime::PlaySoundAtNode("growthSound", player, Cache->SizeReserve/50 * duration, 1.0, "NPC Pelvis [Pelv]");
						Task_FacialEmotionTask_Moan(player, 2.0, "SizeReserve");
						PlayMoanSound(player, Volume);
					}

					float shake_power = std::clamp(Cache->SizeReserve/15 * duration, 0.0f, 2.0f);
					Rumbling::Once("SizeReserve", player, shake_power, 0.05);

					update_target_scale(player, (SizeCalculation/80) * gigantism, SizeEffectType::kNeutral);
					regenerate_health(player, (SizeCalculation/80) * gigantism);

					Cache->SizeReserve -= SizeCalculation/80;
					if (Cache->SizeReserve <= 0) {
						Cache->SizeReserve = 0.0; // Protect against negative values.
					}
				}
			}
		}
	}

	void DisplaySizeReserveEvent(const InputEventData& data) {
		auto player = PlayerCharacter::GetSingleton();
		auto Cache = Persistent::GetSingleton().GetData(player);
		if (Cache) {
			if (Runtime::HasPerk(player, "SizeReserve")) {
				float gigantism = 1.0 + Ench_Aspect_GetPower(player);
				float Value = Cache->SizeReserve * gigantism;
				Notify("Reserved Size: {:.2f}", Value);
			}
		}
	}

	void PartyReportEvent(const InputEventData& data) { // Report follower scale into console
		ReportScale(false);
	}

	void DebugReportEvent(const InputEventData& data) { // Report enemy scale into console
		ReportScale(true);
	}

	void ShrinkOutburstEvent(const InputEventData& data) {

		auto player = PlayerCharacter::GetSingleton();
		bool DarkArts = Runtime::HasPerk(player, "DarkArts");
		if (!DarkArts) {
			return; // no perk, do nothing
		}

		bool DarkArts2 = Runtime::HasPerk(player, "DarkArts_Aug2");
		bool DarkArts3 = Runtime::HasPerk(player, "DarkArts_Aug3");

		float gigantism = 1.0 + Ench_Aspect_GetPower(player);

		float multi = GetDamageResistance(player);

		float healthMax = GetMaxAV(player, ActorValue::kHealth);
		float healthCur = GetAV(player, ActorValue::kHealth);
		float damagehp = 80.0;

		if (DarkArts2) {
			damagehp -= 10; // less hp drain
		}
		if (DarkArts3) {
			damagehp -= 10; // even less hp drain
		}

		damagehp *= multi;
		damagehp /= gigantism;

		if (healthCur < damagehp * 1.10) {
			Notify("Your health is too low");
			return; // don't allow us to die from own shrinking
		}

		static Timer NotifyTimer = Timer(2.0);
		bool OnCooldown = IsActionOnCooldown(player, CooldownSource::Misc_ShrinkOutburst);
		if (OnCooldown) {
			if (NotifyTimer.ShouldRunFrame()) {
				float cooldown = GetRemainingCooldown(player, CooldownSource::Misc_ShrinkOutburst);
				std::string message = std::format("Shrink Outburst is on a cooldown: {:.1f} sec", cooldown);
				shake_camera(player, 0.75, 0.35);
				TiredSound(player, message);
			}
			return;
		}
		ApplyActionCooldown(player, CooldownSource::Misc_ShrinkOutburst);
		DamageAV(player, ActorValue::kHealth, damagehp);
		ShrinkOutburstExplosion(player, false);
	}

	void ProtectSmallOnesEvent(const InputEventData& data) {
		static Timer ProtectTimer = Timer(5.0);
		if (CanPerformAnimation(PlayerCharacter::GetSingleton(), 4) && ProtectTimer.ShouldRunFrame()) {
			bool balance = IsInBalanceMode();
			Utils_ProtectTinies(balance);
		}
	}

	void AnimSpeedUpEvent(const InputEventData& data) {
		AnimationManager::AdjustAnimSpeed(0.045); // Increase speed and power
	}
	void AnimSpeedDownEvent(const InputEventData& data) {
		AnimationManager::AdjustAnimSpeed(-0.045); // Decrease speed and power
	}
	void AnimMaxSpeedEvent(const InputEventData& data) {
		AnimationManager::AdjustAnimSpeed(0.090); // Strongest attack speed buff
	}
}

namespace Gts
{
	void InputFunctions::RegisterEvents() {
		InputManager::RegisterInputEvent("SizeReserve", SizeReserveEvent);
		InputManager::RegisterInputEvent("DisplaySizeReserve", DisplaySizeReserveEvent);
		InputManager::RegisterInputEvent("PartyReport", PartyReportEvent);
		InputManager::RegisterInputEvent("DebugReport", DebugReportEvent);
		InputManager::RegisterInputEvent("AnimSpeedUp", AnimSpeedUpEvent);
		InputManager::RegisterInputEvent("AnimSpeedDown", AnimSpeedDownEvent);
		InputManager::RegisterInputEvent("AnimMaxSpeed", AnimMaxSpeedEvent);
		InputManager::RegisterInputEvent("RapidGrowth", RapidGrowthEvent);
		InputManager::RegisterInputEvent("RapidShrink", RapidShrinkEvent);
		InputManager::RegisterInputEvent("ShrinkOutburst", ShrinkOutburstEvent);
		InputManager::RegisterInputEvent("ProtectSmallOnes", ProtectSmallOnesEvent);

		InputManager::RegisterInputEvent("TotalControlGrow", TotalControlGrowEvent);
		InputManager::RegisterInputEvent("TotalControlShrink", TotalControlShrinkEvent);
		InputManager::RegisterInputEvent("TotalControlGrowOther", TotalControlGrowOtherEvent);
		InputManager::RegisterInputEvent("TotalControlShrinkOther", TotalControlShrinkOtherEvent);
	}
}