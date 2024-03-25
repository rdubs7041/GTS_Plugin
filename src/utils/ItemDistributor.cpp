#include "utils/ItemDistributor.hpp"
#include "utils/actorUtils.hpp"
#include "managers/Rumble.hpp"
#include "data/runtime.hpp"
#include "colliders/RE.hpp"
#include "rays/raycast.hpp"
#include "UI/DebugAPI.hpp"
#include "utils/av.hpp"
#include "profiler.hpp"
#include "timer.hpp"
#include "node.hpp"


#include <vector>
#include <string>


using namespace RE;
using namespace Gts;

namespace {
    const FormID Potion_ResistSize =        0x3D457E;
    const FormID Potion_Growth =            0x3D4580;

    const FormID Potion_ResistSize_Weak =   0x3D9689;

    const FormID Potion_SizeLimit_Weak =    0x3E38B5;
    const FormID Potion_SizeLimit_Normal =  0x3E38B7;
    const FormID Potion_SizeLimit_Strong =  0x3E38B9;
    const FormID Potion_SizeLimit_Extreme = 0x3E38BE;

    const FormID Potion_SizeHunger_Weak =    0x42F7D4;
    const FormID Potion_SizeHunger_Normal =  0x4399DF;
    const FormID Potion_SizeHunger_Strong =  0x4399DD;
    const FormID Potion_SizeHunger_Extreme = 0x4399E1;

    const FormID Potion_Size_Amplify =       0x452F1E;
    const FormID Poison_Size_Drain =         0x5B5553;
    const FormID Poison_Size_Shrink =        0x5B5555;


	// Boss Chests
	const FormID BossChest_Giant = 		0x774BF; // TreastGiantChestBoss
	const FormID BossChest_Bandit = 	0x2064F; // TreasBanditChestBoss
	const FormID BossChest_Draugr =     0x20671; // TreasDraugrChestBoss

    const std::vector<FormID> BossChests = {
        BossChest_Giant,
        BossChest_Bandit,
        BossChest_Draugr,
    };

	// Mini Chests
	const FormID MiniChest_Normal =     0x3AC21;	// TreasBanditChest
	const FormID MiniChest_Giant =      0x774C6;    // TreastGiantChest

    const std::vector<FormID> MiniChests = {
        MiniChest_Normal,
        MiniChest_Giant,
    };

	// Barrels and misc
	const FormID Barrel_1 =     		0x845; 		// Barrel 1
	const FormID Barrel_2 =             0xFE078806; // Barrel 2
	const FormID Long_Crate_1 =         0xFE05D819; // Long Crate 1
	const FormID Long_Crate_2 =         0xFE05D81A; // Long Crate 2

    const std::vector<FormID> MiscChests = {
        Barrel_1,
        Barrel_2,
        Long_Crate_1,
        Long_Crate_2,
    };
}

namespace Gts {
    TESContainer* GetChestRef(FormID form, ChestType type) {
        switch (type) {
            case ChestType::BossChest: {
                for (auto chests: BossChests) {
                    if (chests == form) {
                        return form->As<RE::TESContainer>();
                    }
                }
                break;
            }
            case ChestType::MiniChest: {
                for (auto chests: MiniChests) {
                    if (chests == form) {
                        return form->As<RE::TESContainer>();
                    }
                }
                break;
            }
            case ChestType::MiscChest: {
                for (auto chests: BossChests) {
                    if (chests == form) {
                        return form->As<RE::TESContainer>();
                    }
                }
                break;
            }
        }
        return nullptr;
    }

    void DistributeChestItems() {
        float QuestStage = Runtime::GetStage("MainQuest");
        if (QuestStage < 20) {
            return;
        }
        for (auto Chest: FindAllChests()) {
            if (Chest.size() > 0) {
                AddItemToChests(Chest->formID);
            }
        }
    }

    void AddItemToChests(FormID Chest) {
        TESContainer* container_Boss = GetChestRef(Chest, ChestType::BossChest); 
        TESContainer* container_Mini = GetChestRef(Chest, ChestType::MiniChest); 
        TESContainer* container_Misc = GetChestRef(Chest, ChestType::MiscChest); 
        if (container_Boss) {
            log::info("Boss container found!");
            for (auto item: CalculateItemProbability(ChestType::BossChest)) {
                if (item) {
                    container_Misc->AddObjectToContainer(item, nullptr, 1, nullptr);
                }
            }
        }
        if (container_Mini) {
            log::info("Mini chest found!");
            for (auto item: CalculateItemProbability(ChestType::MiniChest)) {
                if (item) {
                    container_Misc->AddObjectToContainer(item, nullptr, 1, nullptr);
                }
            }
        }
        if (container_Misc) {
            log::info("Misc chest found!");
            for (auto item: CalculateItemProbability(ChestType::MiscChest)) {
                if (item) {
                    container_Misc->AddObjectToContainer(item, nullptr, 1, nullptr);
                }
            }
        }
    }

    std::vector<TESForm*> FindAllChests() {
        RE::TESDataHandler* const DataHandler = RE::TESDataHandler::GetSingleton();

        std::vector<TESForm*> Forms = DataHandler->GetFormArray(RE::FormType::Container);
        
        if (Forms.size() < 1) {
            return {};
        }

        return Forms;
    }

    std::vector<TESBoundObject*> CalculateItemProbability(ChestType type) {
        float HighLootChance = Runtime::GetStage("MainQuest");
        float Level = GetGtsSkillLevel();
        log::info("Item could be added but there's no items yet");
        return {};
    }
}