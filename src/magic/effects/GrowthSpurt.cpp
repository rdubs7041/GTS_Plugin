#include "magic/effects/GrowthSpurt.hpp"
#include "managers/GtsSizeManager.hpp"
#include "managers/GtsManager.hpp"
#include "magic/magic.hpp"
#include "magic/effects/common.hpp"
#include "scale/scale.hpp"
#include "data/runtime.hpp"
#include "timer.hpp"
#include "managers/Rumble.hpp"

namespace {
	void PlayShrinkAudio(Actor* actor, bool timer_1, bool timer_2, float power) {
		float scale = get_visual_scale(actor);
		float falloff = 0.10 * scale;
		GRumble::Once("GrowthSpurt", actor, 7.0, 0.05);
		if (timer_1) {
			Runtime::PlaySoundAtNode_FallOff("xlRumbleL", actor, power/20, 1.0, "NPC Pelvis [Pelv]", falloff);
		}
		if (timer_2) {
			float Volume = clamp(0.10, 1.0, get_visual_scale(actor) * 0.10);
			Runtime::PlaySoundAtNode_FallOff("shrinkSound", actor, Volume, 1.0, "NPC Pelvis [Pelv]", falloff);
		}
	}

	void PlayGrowthAudio(Actor* actor, bool timer_1, bool timer_2, float power) {
		float scale = get_visual_scale(actor);
		float falloff = 0.10 * scale;
		GRumble::Once("GrowthSpurt", actor, scale * 2, 0.05);
		if (timer_1) {
			Runtime::PlaySoundAtNode_FallOff("xlRumbleL", actor, power/20, 1.0, "NPC Pelvis [Pelv]", falloff);
		}
		if (timer_2) {
			float Volume = clamp(0.20, 1.0, scale * 0.10);
			Runtime::PlaySoundAtNode_FallOff("growthSound", actor, Volume, 1.0, "NPC Pelvis [Pelv]", falloff);
		}
	}

	float Get_Perk_Bonus(Actor* giant) {
		float bonus = 1.0;
		float basic = 0.0;
		
		if (Runtime::HasPerk(giant, "ExtraGrowth")) {
			basic += 0.50;
		}
		if (Runtime::HasPerk(giant, "ExtraGrowthMax")) {
			float perkbonus = 1.0 + ((GetGtsSkillLevel() * 0.015) + (giant->GetLevel() * 0.030));
			basic *= perkbonus;
		}
		return bonus + basic;
	}

	float Get_Growth_Limit(Actor* giant, int rank) {
		float basic = 1.0;
		float bonus = 0.0;

		if (rank == 1) {
			bonus = 0.25;
		} else if (rank == 2) {
			bonus = 0.50;
		} else if (rank == 3) {
			bonus = 1.0;
		}

		bonus *= Get_Perk_Bonus(giant);
		bonus *= get_natural_scale(giant, true);
		return basic + bonus;
	}

	void GrowthSpurt_RegenerateAttributes(Actor* caster) {
		float HpRegen = GetMaxAV(caster, ActorValue::kHealth) * 0.00020;
		
		if (Runtime::HasPerk(caster, "HealthRegenPerk")) {
			HpRegen *= 2.0;
		}

		caster->AsActorValueOwner()->RestoreActorValue(ACTOR_VALUE_MODIFIER::kDamage, ActorValue::kHealth, HpRegen * TimeScale());
	}

	float update_target_scale(Actor* giant, float amt, SizeEffectType type) {
		bool perk = Runtime::HasPerkTeam(giant, "OnTheEdge");
		float scale = get_visual_scale(giant);
		float Edge = 1.0;
		
		if (amt > 0 && (giant->formID == 0x14 || IsTeammate(giant))) {
			if (scale >= 1.0) {
				amt /= GetGrowthReduction(scale); // Enabled if BalanceMode is True. Decreases Grow Efficiency.
			}
		} else if (amt - EPS < 0.0) {
			// If neative change: add stolen attributes
			DistributeStolenAttributes(giant, -amt * GetGrowthReduction(scale)); // Adjust max attributes
		}
		if (type == SizeEffectType::kShrink) {
			Edge = GetPerkBonus_OnTheEdge(giant, amt);
		}

		mod_target_scale(giant, amt * Edge); // set target scale value
		return amt * Edge;
	}
}

namespace Gts {
	std::string GrowthSpurt::GetName() {
		return "GrowthSpurt";
	}

	GrowthSpurt::GrowthSpurt(ActiveEffect* effect) : Magic(effect) {
	}

	void GrowthSpurt::OnStart() {
		Actor* caster = GetCaster();
		if (!caster) {
			return;
		}
		this->AllowStacking = true;
	}

	void GrowthSpurt::OnUpdate() {
		Actor* caster = GetCaster();
		if (!caster) {
			return;
		}
		const float GROWTH_1_POWER = 0.00125;
		const float GROWTH_2_POWER = 0.00145;
		const float GROWTH_3_POWER = 0.00175;

		auto base_spell = GetBaseEffect();

		
		if (base_spell == Runtime::GetMagicEffect("GrowthSpurt1")) {
			this->power = GROWTH_1_POWER * Get_Perk_Bonus(caster);
			this->grow_limit = Get_Growth_Limit(caster, 1);
		} else if (base_spell == Runtime::GetMagicEffect("GrowthSpurt2")) {
			this->power = GROWTH_2_POWER * Get_Perk_Bonus(caster);
			this->grow_limit = Get_Growth_Limit(caster, 2);
		} else if (base_spell == Runtime::GetMagicEffect("GrowthSpurt3")) {
			this->power = GROWTH_3_POWER * Get_Perk_Bonus(caster);
			this->grow_limit = Get_Growth_Limit(caster, 3);
		}

		float Gigantism = 1.0 + Ench_Aspect_GetPower(caster);
		float scale = get_target_scale(caster);

		float bonus = 1.0;
		float limit = this->grow_limit * Gigantism;
		float MaxSize = get_max_scale(caster) - 0.004;
		

		GrowthSpurt_RegenerateAttributes(caster);

		if (scale < limit && scale < MaxSize) {
			if (Runtime::HasMagicEffect(PlayerCharacter::GetSingleton(), "EffectSizeAmplifyPotion")) {
				bonus = get_visual_scale(caster) * 0.25 + 0.75;
			}
			DoGrowth(caster, this->power * bonus);
		}
	}

	void GrowthSpurt::OnFinish() {
		Actor* caster = GetCaster();
		if (!caster) {
			return;
		}
		GrowthSpurt::DoShrink(caster);
	}

	void GrowthSpurt::DoGrowth(Actor* actor, float value) {
		float value = update_target_scale(actor, value, SizeEffectType::kGrow); // Grow
		if (SizeManager::GetSingleton().GetGrowthSpurt(actor) < (this->grow_limit - get_natural_scale(actor, true))) {
			if (this->AllowStacking) {
				SizeManager::GetSingleton().ModGrowthSpurt(actor, value);
			}
		} else {
			this->AllowStacking = false;
		}
		PlayGrowthAudio(actor, this->timer.ShouldRun(), this->timerSound.ShouldRunFrame(), this->power);
	}

	void GrowthSpurt::DoShrink(Actor* actor) {
		float value = SizeManager::GetSingleton().GetGrowthSpurt(actor);
		float naturalscale = get_natural_scale(actor, true);
		update_target_scale(actor, -value, SizeEffectType::kNeutral); // Do Shrink
		if (get_target_scale(actor) <= naturalscale) {
			set_target_scale(actor, naturalscale);
		}

		SizeManager::GetSingleton().SetGrowthSpurt(actor, 0.0);

		this->AllowStacking = true;
		PlayShrinkAudio(actor, this->timer.ShouldRun(), this->timerSound.ShouldRunFrame(), this->power);
	}
}
