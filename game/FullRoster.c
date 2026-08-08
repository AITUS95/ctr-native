#include <common.h>

// Native full-roster character select.
//
// Retail has metadata/storage for 16 character IDs but draws only 15 menu
// icons. The old OxideFix proved the remaining transition slot can host Oxide.
// This native implementation keeps the retail menu code, supplies 16-entry
// navigation tables, draws the missing Oxide icon, and loads Oxide's menu model
// from the user's own CTR disc image instead of embedding retail model bytes.

enum
{
	FULL_ROSTER_ICON_COUNT = 16,
	FULL_ROSTER_OXIDE_ICON = 15,
	FULL_ROSTER_LAYOUT_COUNT = 6,
	FULL_ROSTER_RETAIL_EXPANSION_BIT = 9,
	FULL_ROSTER_FAKE_CRASH_BIT = 11,
	FULL_ROSTER_OXIDE_MENU_SCALE_NUM = 5,
	FULL_ROSTER_OXIDE_MENU_SCALE_SHIFT = 3,
};

static struct CharacterSelectMeta s_fullRoster1P2P[FULL_ROSTER_ICON_COUNT] =
{
	{0x80,  0x60, { 0,  4,  8,  1}, CRASH_BANDICOOT, MM_CHARACTER_UNLOCK_ALWAYS},
	{0xC0,  0x60, { 1,  5,  0,  2}, NEO_CORTEX,      MM_CHARACTER_UNLOCK_ALWAYS},
	{0x100, 0x60, { 2,  6,  1,  3}, TINY_TIGER,      MM_CHARACTER_UNLOCK_ALWAYS},
	{0x140, 0x60, { 3,  7,  2,  9}, COCO_BANDICOOT,  MM_CHARACTER_UNLOCK_ALWAYS},
	{0x80,  0x87, { 0, 12, 10,  5}, N_GIN,           MM_CHARACTER_UNLOCK_ALWAYS},
	{0xC0,  0x87, { 1, 13,  4,  6}, DINGODILE,       MM_CHARACTER_UNLOCK_ALWAYS},
	{0x100, 0x87, { 2, 14,  5,  7}, POLAR,           MM_CHARACTER_UNLOCK_ALWAYS},
	{0x140, 0x87, { 3, 15,  6, 11}, PURA,            MM_CHARACTER_UNLOCK_ALWAYS},
	{0x40,  0x60, { 8, 10,  8,  0}, N_TROPY,         MM_CHARACTER_UNLOCK_ALWAYS},
	{0x180, 0x60, { 9, 11,  3,  9}, PINSTRIPE,       MM_CHARACTER_UNLOCK_ALWAYS},
	{0x40,  0x87, { 8, 10, 10,  4}, RIPPER_ROO,      MM_CHARACTER_UNLOCK_ALWAYS},
	{0x180, 0x87, { 9, 15,  7, 11}, PAPU_PAPU,       MM_CHARACTER_UNLOCK_ALWAYS},
	{0x80,  0xAE, { 4, 12, 12, 13}, KOMODO_JOE,      MM_CHARACTER_UNLOCK_ALWAYS},
	{0xC0,  0xAE, { 5, 13, 12, 14}, PENTA_PENGUIN,   MM_CHARACTER_UNLOCK_ALWAYS},
	{0x100, 0xAE, { 6, 14, 13, 15}, FAKE_CRASH,      MM_CHARACTER_UNLOCK_ALWAYS},
	{0x140, 0xAE, { 7, 15, 14, 11}, NITROS_OXIDE,    MM_CHARACTER_UNLOCK_ALWAYS},
};

static struct CharacterSelectMeta s_fullRoster3P[FULL_ROSTER_ICON_COUNT] =
{
	{0x20, 0x47, {12,  4,  0,  1}, CRASH_BANDICOOT, MM_CHARACTER_UNLOCK_ALWAYS},
	{0x60, 0x47, {13,  5,  0,  2}, NEO_CORTEX,      MM_CHARACTER_UNLOCK_ALWAYS},
	{0xA0, 0x47, {14,  6,  1,  3}, TINY_TIGER,      MM_CHARACTER_UNLOCK_ALWAYS},
	{0xE0, 0x47, {15,  7,  2,  3}, COCO_BANDICOOT,  MM_CHARACTER_UNLOCK_ALWAYS},
	{0x20, 0x6E, { 0,  8,  4,  5}, N_GIN,           MM_CHARACTER_UNLOCK_ALWAYS},
	{0x60, 0x6E, { 1,  9,  4,  6}, DINGODILE,       MM_CHARACTER_UNLOCK_ALWAYS},
	{0xA0, 0x6E, { 2, 10,  5,  7}, POLAR,           MM_CHARACTER_UNLOCK_ALWAYS},
	{0xE0, 0x6E, { 3, 11,  6,  7}, PURA,            MM_CHARACTER_UNLOCK_ALWAYS},
	{0x20, 0x95, { 4,  8,  8,  9}, N_TROPY,         MM_CHARACTER_UNLOCK_ALWAYS},
	{0x60, 0x95, { 5,  9,  8, 10}, PINSTRIPE,       MM_CHARACTER_UNLOCK_ALWAYS},
	{0xA0, 0x95, { 6, 10,  9, 11}, RIPPER_ROO,      MM_CHARACTER_UNLOCK_ALWAYS},
	{0xE0, 0x95, { 7, 11, 10, 11}, PAPU_PAPU,       MM_CHARACTER_UNLOCK_ALWAYS},
	{0x20, 0x20, {12,  0, 12, 13}, KOMODO_JOE,      MM_CHARACTER_UNLOCK_ALWAYS},
	{0x60, 0x20, {13,  1, 12, 14}, PENTA_PENGUIN,   MM_CHARACTER_UNLOCK_ALWAYS},
	{0xA0, 0x20, {14,  2, 13, 15}, FAKE_CRASH,      MM_CHARACTER_UNLOCK_ALWAYS},
	{0xE0, 0x20, {15,  3, 14, 15}, NITROS_OXIDE,    MM_CHARACTER_UNLOCK_ALWAYS},
};

static struct CharacterSelectMeta s_fullRoster4P[FULL_ROSTER_ICON_COUNT] =
{
	{0x80,  0x47, { 0,  4, 10,  1}, CRASH_BANDICOOT, MM_CHARACTER_UNLOCK_ALWAYS},
	{0xC0,  0x47, {14,  5,  0,  2}, NEO_CORTEX,      MM_CHARACTER_UNLOCK_ALWAYS},
	{0x100, 0x47, {15,  6,  1,  3}, TINY_TIGER,      MM_CHARACTER_UNLOCK_ALWAYS},
	{0x140, 0x47, { 3,  7,  2, 11}, COCO_BANDICOOT,  MM_CHARACTER_UNLOCK_ALWAYS},
	{0x80,  0x6E, { 0,  4, 12,  5}, N_GIN,           MM_CHARACTER_UNLOCK_ALWAYS},
	{0xC0,  0x6E, { 1,  8,  4,  6}, DINGODILE,       MM_CHARACTER_UNLOCK_ALWAYS},
	{0x100, 0x6E, { 2,  9,  5,  7}, POLAR,           MM_CHARACTER_UNLOCK_ALWAYS},
	{0x140, 0x6E, { 3,  7,  6, 13}, PURA,            MM_CHARACTER_UNLOCK_ALWAYS},
	{0xC0,  0x95, { 5,  8,  8,  9}, N_TROPY,         MM_CHARACTER_UNLOCK_ALWAYS},
	{0x100, 0x95, { 6,  9,  8,  9}, FAKE_CRASH,      MM_CHARACTER_UNLOCK_ALWAYS},
	{0x40,  0x47, {10, 12, 10,  0}, RIPPER_ROO,      MM_CHARACTER_UNLOCK_ALWAYS},
	{0x180, 0x47, {11, 13,  3, 11}, PAPU_PAPU,       MM_CHARACTER_UNLOCK_ALWAYS},
	{0x40,  0x6E, {10, 12, 12,  4}, KOMODO_JOE,      MM_CHARACTER_UNLOCK_ALWAYS},
	{0x180, 0x6E, {11, 13,  7, 13}, PINSTRIPE,       MM_CHARACTER_UNLOCK_ALWAYS},
	{0xC0,  0x20, {14,  1, 14, 15}, PENTA_PENGUIN,   MM_CHARACTER_UNLOCK_ALWAYS},
	{0x100, 0x20, {15,  2, 14, 15}, NITROS_OXIDE,    MM_CHARACTER_UNLOCK_ALWAYS},
};

static struct Level *s_fullRosterModelLevel;
static struct Model *s_fullRosterOxideModel;
static struct Model **s_fullRosterAugmentedModels;

static void FullRoster_InstallTables(void)
{
	D230.characterSelectMetaByLayout[0] = s_fullRoster1P2P;
	D230.characterSelectMetaByLayout[1] = s_fullRoster1P2P;
	D230.characterSelectMetaByLayout[2] = s_fullRoster3P;
	D230.characterSelectMetaByLayout[3] = s_fullRoster4P;
	D230.characterSelectMetaByLayout[4] = s_fullRoster1P2P;
	D230.characterSelectMetaByLayout[5] = s_fullRoster1P2P;

	// Retail's map already has room for 16 character IDs; its RestoreIDs loop
	// simply stops at 15 because Oxide has no stock menu icon.
	D230.characterMenuID[NITROS_OXIDE] = FULL_ROSTER_OXIDE_ICON;
}

static u32 FullRoster_EnableRetailExpandedLayout(void)
{
	u32 saved = (u32)sdata->gameProgress.unlocks[0];

	// Set only character bits temporarily. Bit 9 makes retail's layout chooser
	// select the expanded 1P/2P grid; bit 11 suppresses the stock 4P title that
	// shares transition slot 15 with our Oxide icon. The value is restored before
	// returning, so tracks/progression/save data are not modified.
	sdata->gameProgress.unlocks[0] = (int)(saved |
	    (1u << FULL_ROSTER_RETAIL_EXPANSION_BIT) |
	    (1u << FULL_ROSTER_FAKE_CRASH_BIT));

	return saved;
}

static void FullRoster_RestoreUnlockWord(u32 saved)
{
	sdata->gameProgress.unlocks[0] = (int)saved;
}

static b32 FullRoster_ModelNameMatches(struct Model *model, const char *name)
{
	if (model == NULL || name == NULL)
	{
		return false;
	}

	for (s32 word = 0; word < MODEL_NAME_WORD_COUNT; word++)
	{
		if (ModelName_ReadWord(model->name, word) != ModelName_ReadWord(name, word))
		{
			return false;
		}
	}

	return true;
}

static void FullRoster_ResetModelCacheIfNeeded(struct Level *level)
{
	if (s_fullRosterModelLevel == level)
	{
		return;
	}

	s_fullRosterModelLevel = level;
	s_fullRosterOxideModel = NULL;
	s_fullRosterAugmentedModels = NULL;
}

static void FullRoster_ScaleOxideForMenu(struct Model *model)
{
	if (model == NULL || model->headers == NULL || model->numHeaders <= 0)
	{
		return;
	}

	for (s32 headerIndex = 0; headerIndex < model->numHeaders; headerIndex++)
	{
		struct ModelHeader *header = &model->headers[headerIndex];
		header->scale.x = (s16)(((s32)header->scale.x * FULL_ROSTER_OXIDE_MENU_SCALE_NUM) >> FULL_ROSTER_OXIDE_MENU_SCALE_SHIFT);
		header->scale.y = (s16)(((s32)header->scale.y * FULL_ROSTER_OXIDE_MENU_SCALE_NUM) >> FULL_ROSTER_OXIDE_MENU_SCALE_SHIFT);
		header->scale.z = (s16)(((s32)header->scale.z * FULL_ROSTER_OXIDE_MENU_SCALE_NUM) >> FULL_ROSTER_OXIDE_MENU_SCALE_SHIFT);
	}
}

static void FullRoster_EnsureOxideMenuModel(void)
{
	struct GameTracker *gGT = sdata->gGT;
	if (gGT == NULL || gGT->level1 == NULL || sdata->ptrBigfile1 == NULL)
	{
		return;
	}

	struct Level *level = gGT->level1;
	FullRoster_ResetModelCacheIfNeeded(level);

	if (s_fullRosterAugmentedModels != NULL && level->ptrModelsPtrArray == s_fullRosterAugmentedModels)
	{
		return;
	}

	struct Model **models = level->ptrModelsPtrArray;
	if (models == NULL)
	{
		return;
	}

	const char *oxideName = data.MetaDataCharacters[NITROS_OXIDE].name_Debug;
	s32 modelCount = 0;
	while (models[modelCount] != NULL)
	{
		if (FullRoster_ModelNameMatches(models[modelCount], oxideName))
		{
			s_fullRosterOxideModel = models[modelCount];
			return;
		}
		modelCount++;
	}

	if (s_fullRosterOxideModel == NULL)
	{
		u32 fileSize = 0;
		void *fileBase = LOAD_DramFile(
		    sdata->ptrBigfile1,
		    BI_RACERMODELHI + NITROS_OXIDE,
		    NULL,
		    &fileSize,
		    -1);

		if (fileBase == NULL || fileSize <= LOAD_MODEL_FILE_HEADER_BYTES)
		{
			return;
		}

		s_fullRosterOxideModel = (struct Model *)((u8 *)fileBase + LOAD_MODEL_FILE_HEADER_BYTES);
		FullRoster_ScaleOxideForMenu(s_fullRosterOxideModel);
	}

	struct Model **augmented = MEMPACK_AllocMem((modelCount + 2) * (s32)sizeof(*augmented));
	if (augmented == NULL)
	{
		return;
	}

	for (s32 modelIndex = 0; modelIndex < modelCount; modelIndex++)
	{
		augmented[modelIndex] = models[modelIndex];
	}
	augmented[modelCount] = s_fullRosterOxideModel;
	augmented[modelCount + 1] = NULL;

	s_fullRosterAugmentedModels = augmented;
	level->ptrModelsPtrArray = augmented;
}

static void FullRoster_DrawOxideIcon(void)
{
	struct GameTracker *gGT = sdata->gGT;
	if (gGT == NULL || gGT->backBuffer == NULL || gGT->ptrIcons == NULL || D230.activeCharacterSelectMeta == NULL ||
	    D230.characterSelectTransitionMeta == NULL)
	{
		return;
	}

	struct CharacterSelectMeta *meta = &D230.activeCharacterSelectMeta[FULL_ROSTER_OXIDE_ICON];
	struct TransitionMeta *transition = &D230.characterSelectTransitionMeta[FULL_ROSTER_OXIDE_ICON];
	RECT rect;

	rect.x = transition->currX + meta->posX;
	rect.y = transition->currY + meta->posY;
	rect.w = MM_CHARACTER_SELECT_ICON_RECT_W;
	rect.h = MM_CHARACTER_SELECT_ICON_RECT_H;
	RECTMENU_DrawInnerRect(&rect, 0, gGT->backBuffer->otMem.uiOT);

	Color iconColor = D230.characterSelect_NeutralColor;
	for (s32 playerIndex = 0; playerIndex < gGT->numPlyrNextGame; playerIndex++)
	{
		b32 selected = (((s32)(s16)sdata->characterSelectFlags >> playerIndex) & 1U) != 0;
		if (selected && data.characterIDs[playerIndex] == NITROS_OXIDE)
		{
			iconColor = D230.characterSelect_ChosenColor;
			break;
		}
	}

	RECTMENU_DrawPolyGT4(
	    gGT->ptrIcons[data.MetaDataCharacters[NITROS_OXIDE].iconID],
	    transition->currX + meta->posX + MM_CHARACTER_SELECT_ICON_DECAL_OFFSET_X,
	    transition->currY + meta->posY + MM_CHARACTER_SELECT_ICON_DECAL_OFFSET_Y,
	    &gGT->backBuffer->primMem,
	    gGT->pushBuffer_UI.ptrOT,
	    iconColor.self,
	    iconColor.self,
	    iconColor.self,
	    iconColor.self,
	    TRANS_50_DECAL,
	    FP(1.0));
}

void MM_Characters_RestoreIDs(void)
{
	FullRoster_InstallTables();
	u32 savedUnlockWord = FullRoster_EnableRetailExpandedLayout();
	MM_Characters_RestoreIDs_Retail();
	FullRoster_RestoreUnlockWord(savedUnlockWord);
	FullRoster_InstallTables();
}

void MM_Characters_MenuProc(struct RectMenu *menu)
{
	FullRoster_InstallTables();
	FullRoster_EnsureOxideMenuModel();

	u32 savedUnlockWord = FullRoster_EnableRetailExpandedLayout();
	MM_Characters_MenuProc_Retail(menu);
	FullRoster_RestoreUnlockWord(savedUnlockWord);

	// Retail loops over 15 icons, but all of its navigation/selection code can
	// already address slot 15 once activeCharacterSelectMeta points at our table.
	// Only the visual for the sixteenth icon needs to be appended here.
	FullRoster_InstallTables();
	FullRoster_DrawOxideIcon();
}
